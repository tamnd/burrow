#!/bin/sh
# Regenerates tests/image_gif_test_gen.h: what Go's image/gif makes of random
# images and of damaged files. The header has three tables. The first is seeds
# for Encode: the generator and tests/image_gif_test.c both use each one to
# build an image of a random type, size and content and to pick the options,
# and the case holds a digest of the GIF Go writes and what DecodeAll makes of
# it. The second is seeds for EncodeAll, each an animation of a few frames with
# random palettes, bounds, delays, disposal methods and global palette. The
# third is the GIF files from Go's testdata and two made here, each with seeds
# that flip bytes or cut the file short, and what Decode, DecodeAll and
# DecodeConfig say about the result, and what they say when the file comes one
# byte a read where that is different. Nothing depends on the machine, so this
# runs wherever go does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/image_gif_test_gen.h"
testdata="$(go env GOROOT)/src/image/testdata"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"compress/lzw"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/gif"
	"io"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"testing/iotest"
)

type xs struct{ x uint64 }

func (r *xs) next() uint64 {
	r.x ^= r.x << 13
	r.x ^= r.x >> 7
	r.x ^= r.x << 17
	return r.x
}

func (r *xs) intn(n int) int { return int(r.next() % uint64(n)) }

func xcolor(r *xs, k int) color.Color {
	var v [4]uint64
	for i := range v {
		v[i] = r.next()
	}
	b := func(i int) uint8 { return uint8(v[i]) }
	w := func(i int) uint16 { return uint16(v[i]) }
	switch k {
	case 0:
		return color.RGBA{b(0), b(1), b(2), b(3)}
	case 1:
		return color.RGBA64{w(0), w(1), w(2), w(3)}
	case 2:
		return color.NRGBA{b(0), b(1), b(2), b(3)}
	case 3:
		return color.NRGBA64{w(0), w(1), w(2), w(3)}
	case 4:
		return color.Alpha{b(0)}
	case 5:
		return color.Alpha16{w(0)}
	case 6:
		return color.Gray{b(0)}
	case 7:
		return color.Gray16{w(0)}
	case 8:
		return color.YCbCr{b(0), b(1), b(2)}
	case 9:
		return color.NYCbCrA{color.YCbCr{b(0), b(1), b(2)}, b(3)}
	}
	return color.CMYK{b(0), b(1), b(2), b(3)}
}

func fill(r *xs, b []byte) {
	for i := range b {
		b[i] = byte(r.next())
	}
}

// Sets every n-byte alpha sample at offset off in each bpp-byte pixel to all
// ones.
func opaqueFill(b []byte, bpp, off, n int) {
	for i := 0; i+bpp <= len(b); i += bpp {
		for k := 0; k < n; k++ {
			b[i+off+k] = 0xff
		}
	}
}

// An image with only the three methods of image.Image.
type plain struct{ m image.Image }

func (p plain) ColorModel() color.Model { return p.m.ColorModel() }
func (p plain) Bounds() image.Rectangle { return p.m.Bounds() }
func (p plain) At(x, y int) color.Color { return p.m.At(x, y) }

// A PalettedImage that is not an *image.Paletted.
type plainPal struct{ m *image.Paletted }

func (p plainPal) ColorModel() color.Model       { return p.m.ColorModel() }
func (p plainPal) Bounds() image.Rectangle       { return p.m.Bounds() }
func (p plainPal) At(x, y int) color.Color       { return p.m.At(x, y) }
func (p plainPal) ColorIndexAt(x, y int) uint8   { return p.m.ColorIndexAt(x, y) }

func xpalette(r *xs) color.Palette {
	var n int
	switch r.intn(5) {
	case 0:
		n = 1 + r.intn(2)
	case 1:
		n = 3 + r.intn(2)
	case 2:
		n = 5 + r.intn(12)
	case 3:
		n = 17 + r.intn(240)
	default:
		if r.intn(3) == 0 {
			n = 257 + r.intn(8)
		} else {
			n = 1 + r.intn(256)
		}
	}
	p := make(color.Palette, n)
	for i := range p {
		p[i] = xcolor(r, r.intn(11))
	}
	return p
}

// build: an image of kind k, as tests/image_gif_test.c builds it.
func build(r *xs) image.Image {
	k := r.intn(14)
	x0, y0 := r.intn(9)-4, r.intn(9)-4
	inner := k
	if k == 12 {
		inner = r.intn(9)
	}
	if inner == 10 || inner == 11 {
		// Go's YCbCr offsets divide negative coordinates the wrong way,
		// so keep these where they do not.
		x0, y0 = x0+4, y0+4
	}
	rect := image.Rect(x0, y0, x0+r.intn(40), y0+r.intn(40))
	opaque := r.intn(2) == 0
	var m image.Image
	switch inner {
	case 0:
		p := image.NewRGBA(rect)
		fill(r, p.Pix)
		if opaque {
			opaqueFill(p.Pix, 4, 3, 1)
		}
		m = p
	case 1:
		p := image.NewRGBA64(rect)
		fill(r, p.Pix)
		if opaque {
			opaqueFill(p.Pix, 8, 6, 2)
		}
		m = p
	case 2:
		p := image.NewNRGBA(rect)
		fill(r, p.Pix)
		if opaque {
			opaqueFill(p.Pix, 4, 3, 1)
		}
		m = p
	case 3:
		p := image.NewNRGBA64(rect)
		fill(r, p.Pix)
		if opaque {
			opaqueFill(p.Pix, 8, 6, 2)
		}
		m = p
	case 4:
		p := image.NewAlpha(rect)
		fill(r, p.Pix)
		if opaque {
			opaqueFill(p.Pix, 1, 0, 1)
		}
		m = p
	case 5:
		p := image.NewAlpha16(rect)
		fill(r, p.Pix)
		if opaque {
			opaqueFill(p.Pix, 2, 0, 2)
		}
		m = p
	case 6:
		p := image.NewGray(rect)
		fill(r, p.Pix)
		m = p
	case 7:
		p := image.NewGray16(rect)
		fill(r, p.Pix)
		m = p
	case 8:
		p := image.NewCMYK(rect)
		fill(r, p.Pix)
		m = p
	case 9, 13:
		p := image.NewPaletted(rect, xpalette(r))
		fill(r, p.Pix)
		// At panics on an index past the palette, so keep them inside.
		for i := range p.Pix {
			p.Pix[i] = byte(int(p.Pix[i]) % len(p.Palette))
		}
		m = p
	case 10:
		p := image.NewYCbCr(rect, image.YCbCrSubsampleRatio(r.intn(6)))
		fill(r, p.Y)
		fill(r, p.Cb)
		fill(r, p.Cr)
		m = p
	case 11:
		p := image.NewNYCbCrA(rect, image.YCbCrSubsampleRatio(r.intn(6)))
		fill(r, p.Y)
		fill(r, p.Cb)
		fill(r, p.Cr)
		fill(r, p.A)
		if opaque {
			opaqueFill(p.A, 1, 0, 1)
		}
		m = p
	}
	if r.intn(3) == 0 && !rect.Empty() {
		sx0 := x0 + r.intn(rect.Dx())
		sy0 := y0 + r.intn(rect.Dy())
		sx1 := sx0 + r.intn(rect.Max.X-sx0+1)
		sy1 := sy0 + r.intn(rect.Max.Y-sy0+1)
		m = m.(interface {
			SubImage(image.Rectangle) image.Image
		}).SubImage(image.Rect(sx0, sy0, sx1, sy1))
	}
	switch k {
	case 12:
		m = plain{m}
	case 13:
		m = plainPal{m.(*image.Paletted)}
	}
	return m
}

func fnv(b []byte) uint64 {
	h := uint64(0xcbf29ce484222325)
	for _, c := range b {
		h ^= uint64(c)
		h *= 0x100000001b3
	}
	return h
}

func paletteDigest(p color.Palette) string {
	var b []byte
	for _, c := range p {
		switch c.(type) {
		case color.RGBA:
			b = append(b, 'r')
		case color.NRGBA:
			b = append(b, 'n')
		default:
			b = append(b, '?')
		}
		r, g, bl, a := c.RGBA()
		b = binary.BigEndian.AppendUint32(b, r)
		b = binary.BigEndian.AppendUint32(b, g)
		b = binary.BigEndian.AppendUint32(b, bl)
		b = binary.BigEndian.AppendUint32(b, a)
	}
	return fmt.Sprintf("p%d:%016x", len(p), fnv(b))
}

func reader(data []byte, slow bool) io.Reader {
	if slow {
		return iotest.OneByteReader(bytes.NewReader(data))
	}
	return bytes.NewReader(data)
}

func frameSummary(m *image.Paletted) string {
	b := m.Bounds()
	return fmt.Sprintf("%d,%d,%d,%d %016x %s", b.Min.X, b.Min.Y, b.Max.X, b.Max.Y,
		fnv(m.Pix), paletteDigest(m.Palette))
}

func decodeSummary(data []byte, slow bool) string {
	m, err := gif.Decode(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	return frameSummary(m.(*image.Paletted))
}

func allSummary(g *gif.GIF) string {
	s := fmt.Sprintf("%dx%d %s loop %d bg %d", g.Config.Width, g.Config.Height,
		paletteDigest(g.Config.ColorModel.(color.Palette)), g.LoopCount, g.BackgroundIndex)
	for i, m := range g.Image {
		s += fmt.Sprintf(" [%s d%d s%d]", frameSummary(m), g.Delay[i], g.Disposal[i])
	}
	return s
}

func decodeAllSummary(data []byte, slow bool) string {
	g, err := gif.DecodeAll(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	return allSummary(g)
}

func configSummary(data []byte, slow bool) string {
	c, err := gif.DecodeConfig(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	return fmt.Sprintf("%dx%d %s", c.Width, c.Height, paletteDigest(c.ColorModel.(color.Palette)))
}

// grayQuantizer fills the palette it is given with evenly spaced grays, as
// many as it has room for.
type grayQuantizer struct{}

func (grayQuantizer) Quantize(p color.Palette, m image.Image) color.Palette {
	n := cap(p)
	for i := 0; i < n; i++ {
		v := 255
		if n > 1 {
			v = i * 255 / (n - 1)
		}
		p = append(p, color.Gray{uint8(v)})
	}
	return p
}

func encoded(buf *bytes.Buffer, err error) string {
	if err != nil {
		return "E:" + err.Error()
	}
	return fmt.Sprintf("%d:%016x | %s", buf.Len(), fnv(buf.Bytes()),
		decodeAllSummary(buf.Bytes(), false))
}

func encodeCase(seed uint64) string {
	r := &xs{seed}
	m := build(r)
	var o gif.Options
	switch r.intn(4) {
	case 1:
		o.NumColors = 1 + r.intn(256)
	case 2:
		o.NumColors = 1 + r.intn(16)
	case 3:
		o.NumColors = 300
	}
	switch r.intn(3) {
	case 1:
		o.Drawer = draw.FloydSteinberg
	case 2:
		o.Drawer = draw.Src
	}
	if r.intn(3) == 0 {
		o.Quantizer = grayQuantizer{}
	}
	var buf bytes.Buffer
	return encoded(&buf, gif.Encode(&buf, m, &o))
}

// gifPalette: 1 to 16 or 1 to 256 colors of any type, sometimes with a
// transparent one.
func gifPalette(r *xs) color.Palette {
	n := 1 + r.intn(16)
	if r.intn(2) == 0 {
		n = 1 + r.intn(256)
	}
	p := make(color.Palette, n)
	for i := range p {
		p[i] = xcolor(r, r.intn(11))
	}
	if r.intn(4) == 0 {
		p[r.intn(n)] = color.RGBA{}
	}
	return p
}

func frame(r *xs, w, h int, p color.Palette) *image.Paletted {
	x0, y0 := r.intn(w), r.intn(h)
	x1, y1 := x0+r.intn(w-x0+1), y0+r.intn(h-y0+1)
	if r.intn(10) == 0 {
		x1 += 1 + r.intn(3)
	}
	m := image.NewPaletted(image.Rect(x0, y0, x1, y1), p)
	if r.intn(10) == 0 {
		fill(r, m.Pix)
	} else {
		for i := range m.Pix {
			m.Pix[i] = byte(r.intn(len(p)))
		}
	}
	return m
}

func buildAll(r *xs) *gif.GIF {
	w, h := 1+r.intn(40), 1+r.intn(40)
	n := 1 + r.intn(4)
	g := &gif.GIF{LoopCount: r.intn(5) - 1, BackgroundIndex: byte(r.intn(4))}
	mode := r.intn(6)
	var global color.Palette
	switch mode {
	case 1, 2, 3:
		global = gifPalette(r)
	}
	for i := 0; i < n; i++ {
		p := global
		if mode == 1 || mode == 3 || (mode == 2 && i > 0 && r.intn(2) == 0) || global == nil {
			p = gifPalette(r)
		}
		g.Image = append(g.Image, frame(r, w, h, p))
		d := 0
		if r.intn(3) != 0 {
			d = r.intn(500)
		}
		g.Delay = append(g.Delay, d)
	}
	if mode == 3 {
		// A copy of the first frame's palette, which the encoder finds
		// by comparing the tables.
		global = append(color.Palette(nil), g.Image[0].Palette...)
	}
	if r.intn(2) == 0 {
		for i := 0; i < n; i++ {
			g.Disposal = append(g.Disposal, byte(r.intn(4)))
		}
	}
	switch mode {
	case 1, 2, 3:
		g.Config = image.Config{ColorModel: global, Width: w, Height: h}
	case 4:
		g.Config = image.Config{Width: w, Height: h}
	case 5:
		if r.intn(4) == 0 {
			g.Config = image.Config{ColorModel: color.RGBAModel, Width: w, Height: h}
		}
	}
	switch r.intn(20) {
	case 0:
		g.Delay = append(g.Delay, 0)
	case 1:
		g.Disposal = make([]byte, n+1)
	}
	return g
}

func encodeAllCase(seed uint64) string {
	g := buildAll(&xs{seed})
	var buf bytes.Buffer
	return encoded(&buf, gif.EncodeAll(&buf, g))
}

func mutate(data []byte, seed uint64) []byte {
	b := append([]byte(nil), data...)
	if seed == 0 {
		return b
	}
	r := &xs{seed}
	if r.intn(3) == 2 {
		return b[:r.intn(len(b))]
	}
	n := 1 + r.intn(3)
	for i := 0; i < n; i++ {
		pos := 6 + r.intn(len(b)-6)
		b[pos] ^= byte(1 + r.intn(255))
	}
	return b
}

// blocks writes data as sub-blocks and the terminator.
func blocks(w *bytes.Buffer, data []byte) {
	for len(data) > 0 {
		n := min(len(data), 255)
		w.WriteByte(byte(n))
		w.Write(data[:n])
		data = data[n:]
	}
	w.WriteByte(0)
}

// extras: an interlaced two-frame GIF with every kind of extension Go reads,
// a NETSCAPE block that sets no loop count and a stray byte after the image
// data, which Go allows.
func extras() []byte {
	var b bytes.Buffer
	b.WriteString("GIF87a")
	b.Write([]byte{11, 0, 13, 0, 0x80 | 1, 2, 0})
	b.Write([]byte{0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255})
	b.Write([]byte{0x21, 0xfe})
	blocks(&b, []byte("a comment"))
	b.Write([]byte{0x21, 0x01, 12})
	b.Write(make([]byte, 12))
	blocks(&b, []byte("plain text"))
	b.Write([]byte{0x21, 0xff, 11})
	b.WriteString("XMP DataXMP")
	blocks(&b, []byte("<xmp/>"))
	b.Write([]byte{0x21, 0xff, 11})
	b.WriteString("NETSCAPE2.0")
	b.Write([]byte{5, 2, 0, 0, 0, 0, 0})
	for f := 0; f < 2; f++ {
		b.Write([]byte{0x21, 0xf9, 4, 1 | byte(f+1)<<2, 7, 0, 3, 0})
		w, h := 11-f*3, 13-f*4
		b.Write([]byte{0x2c, byte(f), 0, byte(f * 2), 0, byte(w), 0, byte(h), 0, 0x40})
		pix := make([]byte, w*h)
		for i := range pix {
			pix[i] = byte((i*7 + f) % 4)
		}
		var z bytes.Buffer
		lw := lzw.NewWriter(&z, lzw.LSB, 2)
		lw.Write(pix)
		lw.Close()
		b.WriteByte(2)
		data := z.Bytes()
		if f == 1 {
			data = append(data, 0)
		}
		blocks(&b, data)
	}
	b.WriteByte(0x3b)
	return b.Bytes()
}

// animated: what EncodeAll writes for three frames, with a loop count, local
// palettes, a transparent color and disposal methods.
func animated() []byte {
	pal := color.Palette{color.RGBA{0, 0, 0, 255}, color.RGBA{255, 255, 255, 255},
		color.RGBA{}, color.RGBA{255, 0, 0, 255}, color.RGBA{0, 0, 255, 255}}
	g := &gif.GIF{LoopCount: 3, Config: image.Config{ColorModel: pal, Width: 20, Height: 16},
		BackgroundIndex: 2}
	for f := 0; f < 3; f++ {
		p := pal
		if f == 1 {
			p = color.Palette{color.Gray{9}, color.Gray{200}, color.Gray{77}}
		}
		m := image.NewPaletted(image.Rect(f*2, f, 20-f, 16-f*2), p)
		for i := range m.Pix {
			m.Pix[i] = byte((i/3 + f) % len(p))
		}
		g.Image = append(g.Image, m)
		g.Delay = append(g.Delay, 10*f)
		g.Disposal = append(g.Disposal, byte(f+1))
	}
	var buf bytes.Buffer
	if err := gif.EncodeAll(&buf, g); err != nil {
		panic(err)
	}
	return buf.Bytes()
}

func main() {
	dir := os.Args[1]
	var names []string
	var datas [][]byte
	for _, name := range []string{"triangle-001.gif", "video-001.gif", "video-001.5bpp.gif",
		"video-001.interlaced.gif", "video-005.gray.gif"} {
		data, err := os.ReadFile(filepath.Join(dir, name))
		if err != nil {
			panic(err)
		}
		names = append(names, name)
		datas = append(datas, data)
	}
	names = append(names, "animated", "extras")
	datas = append(datas, animated(), extras())

	fmt.Println("/* Generated by tools/gen-image-gif-tests.sh from Go's image/gif and the")
	fmt.Println(" * GIF files in its testdata. Do not edit. */")
	fmt.Println()
	fmt.Println("static const GifEncodeCase gif_encode_cases[] = {")
	seed := uint64(0x2545f4914f6cdd1d)
	for i := 0; i < 1500; i++ {
		seed += 0x632be59bd9b4e019
		fmt.Printf("    {UINT64_C(%#x), %s},\n", seed, strconv.Quote(encodeCase(seed)))
	}
	fmt.Println("};")
	fmt.Println()
	fmt.Println("static const GifEncodeCase gif_encode_all_cases[] = {")
	for i := 0; i < 1500; i++ {
		seed += 0x632be59bd9b4e019
		fmt.Printf("    {UINT64_C(%#x), %s},\n", seed, strconv.Quote(encodeAllCase(seed)))
	}
	fmt.Println("};")
	fmt.Println()

	for i, data := range datas {
		fmt.Printf("static const Byte gif_file_%d[] = {", i)
		for j, c := range data {
			if j%16 == 0 {
				fmt.Print("\n    ")
			}
			fmt.Printf("%#02x,", c)
		}
		fmt.Println("\n};")
	}
	fmt.Println()
	fmt.Println("static const GifFile gif_files[] = {")
	for i, name := range names {
		fmt.Printf("    {%s, gif_file_%d, sizeof gif_file_%d},\n", strconv.Quote(name), i, i)
	}
	fmt.Println("};")
	fmt.Println()
	fmt.Println("static const GifDecodeCase gif_decode_cases[] = {")
	seed = uint64(0x9e3779b97f4a7c15)
	for i, data := range datas {
		for j := 0; j < 60; j++ {
			s := uint64(0)
			if j > 0 {
				seed += 0x632be59bd9b4e019
				s = seed
			}
			b := mutate(data, s)
			if c, err := gif.DecodeConfig(bytes.NewReader(b)); err == nil &&
				int64(c.Width)*int64(c.Height) > 1<<24 {
				// A flipped size byte can let a frame ask for gigabytes,
				// which a sanitizer build will not hand out.
				continue
			}
			res := decodeSummary(b, false) + " | " + decodeAllSummary(b, false) + " | " +
				configSummary(b, false)
			slow := decodeSummary(b, true) + " | " + decodeAllSummary(b, true) + " | " +
				configSummary(b, true)
			if slow == res {
				slow = "NULL"
			} else {
				slow = strconv.Quote(slow)
			}
			fmt.Printf("    {%d, UINT64_C(%#x), %s, %s},\n", i, s, strconv.Quote(res), slow)
		}
	}
	fmt.Println("};")
	_ = strings.TrimSpace
	_ = binary.BigEndian
}
GO

cd "$tmp"
go mod init gen >/dev/null 2>&1
go run . "$testdata" > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
