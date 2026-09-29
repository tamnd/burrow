#!/bin/sh
# Regenerates tests/image_jpeg_test_gen.h: what Go's image/jpeg makes of
# random images and of damaged files. The header has two tables. The first is
# seeds for Encode: the generator and tests/image_jpeg_test.c both use each one
# to build an image of a random type, size and content, as the gif and png
# tests do, and to pick the quality, and the case holds the length and a digest
# of the JPEG Go writes and what Decode makes of it. The second is the JPEG
# files from Go's testdata and three made here from the CMYK one, each with
# seeds that flip bytes or cut the file short, and what Decode and DecodeConfig
# say about the result, and what they say when the file comes one byte a read
# where that is different. Nothing depends on the machine, so this runs
# wherever go does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/image_jpeg_test_gen.h"
testdata="$(go env GOROOT)/src/image/testdata"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strconv"
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

func reader(data []byte, slow bool) io.Reader {
	if slow {
		return iotest.OneByteReader(bytes.NewReader(data))
	}
	return bytes.NewReader(data)
}

func rect(r image.Rectangle) string {
	return fmt.Sprintf("%d,%d,%d,%d", r.Min.X, r.Min.Y, r.Max.X, r.Max.Y)
}

func plane(b []byte) string { return fmt.Sprintf("%d:%016x", len(b), fnv(b)) }

func imageSummary(m image.Image) string {
	switch m := m.(type) {
	case *image.Gray:
		return fmt.Sprintf("gray %s %d %s", rect(m.Rect), m.Stride, plane(m.Pix))
	case *image.RGBA:
		return fmt.Sprintf("rgba %s %d %s", rect(m.Rect), m.Stride, plane(m.Pix))
	case *image.CMYK:
		return fmt.Sprintf("cmyk %s %d %s", rect(m.Rect), m.Stride, plane(m.Pix))
	case *image.YCbCr:
		return fmt.Sprintf("ycbcr %s %d %d %d %s %s %s", rect(m.Rect), int(m.SubsampleRatio),
			m.YStride, m.CStride, plane(m.Y), plane(m.Cb), plane(m.Cr))
	}
	return fmt.Sprintf("? %T", m)
}

func decodeSummary(data []byte, slow bool) (s string) {
	defer func() {
		if r := recover(); r != nil {
			s = fmt.Sprint("P:", r)
		}
	}()
	m, err := jpeg.Decode(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	return imageSummary(m)
}

func configSummary(data []byte, slow bool) (s string) {
	defer func() {
		if r := recover(); r != nil {
			s = fmt.Sprint("P:", r)
		}
	}()
	c, err := jpeg.DecodeConfig(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	model := "?"
	switch c.ColorModel {
	case color.GrayModel:
		model = "gray"
	case color.YCbCrModel:
		model = "ycbcr"
	case color.RGBAModel:
		model = "rgba"
	case color.CMYKModel:
		model = "cmyk"
	}
	return fmt.Sprintf("%dx%d %s", c.Width, c.Height, model)
}

func encodeCase(seed uint64) string {
	r := &xs{seed}
	m := build(r)
	var o *jpeg.Options
	switch r.intn(4) {
	case 1:
		o = &jpeg.Options{Quality: 1 + r.intn(100)}
	case 2:
		o = &jpeg.Options{Quality: r.intn(300) - 100}
	case 3:
		o = &jpeg.Options{}
	}
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, m, o); err != nil {
		return "E:" + err.Error()
	}
	return fmt.Sprintf("%d:%016x | %s", buf.Len(), fnv(buf.Bytes()),
		decodeSummary(buf.Bytes(), false))
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
		pos := 2 + r.intn(len(b)-2)
		b[pos] ^= byte(1 + r.intn(255))
	}
	return b
}

// huge: whether anything in b that looks like a frame header asks for more
// than 1<<22 pixels, which a sanitizer build will not hand out.
func huge(b []byte) bool {
	for i := 0; i+9 <= len(b); i++ {
		if b[i] == 0xff && b[i+1] >= 0xc0 && b[i+1] <= 0xc2 {
			h := int(b[i+5])<<8 | int(b[i+6])
			w := int(b[i+7])<<8 | int(b[i+8])
			if (w+32)*(h+32) > 1<<22 {
				return true
			}
		}
	}
	return false
}

// patchAdobe: the CMYK file with its APP14 transform byte set to t, or with
// the APP14 marker renamed to APP13 when t is negative.
func patchAdobe(data []byte, t int) []byte {
	b := append([]byte(nil), data...)
	i := bytes.Index(b, []byte("Adobe")) - 4
	if i < 0 || b[i] != 0xff || b[i+1] != 0xee {
		panic("no APP14")
	}
	if t < 0 {
		b[i+1] = 0xed
	} else {
		b[i+4+11] = byte(t)
	}
	return b
}

func main() {
	dir := os.Args[1]
	files, err := filepath.Glob(filepath.Join(dir, "*.jpeg"))
	if err != nil {
		panic(err)
	}
	sort.Strings(files)
	var names []string
	var datas [][]byte
	var cmyk []byte
	for _, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			panic(err)
		}
		name := filepath.Base(f)
		names = append(names, name)
		datas = append(datas, data)
		if name == "video-001.cmyk.jpeg" {
			cmyk = data
		}
	}
	names = append(names, "cmyk-as-ycbcrk", "cmyk-as-rgb-transform", "cmyk-without-adobe")
	datas = append(datas, patchAdobe(cmyk, 2), patchAdobe(cmyk, 1), patchAdobe(cmyk, -1))

	fmt.Println("/* Generated by tools/gen-image-jpeg-tests.sh from Go's image/jpeg and the")
	fmt.Println(" * JPEG files in its testdata. Do not edit. */")
	fmt.Println()
	fmt.Println("static const JpegEncodeCase jpeg_encode_cases[] = {")
	seed := uint64(0x2545f4914f6cdd1d)
	for i := 0; i < 1500; i++ {
		seed += 0x632be59bd9b4e019
		fmt.Printf("    {UINT64_C(%#x), %s},\n", seed, strconv.Quote(encodeCase(seed)))
	}
	fmt.Println("};")
	fmt.Println()

	for i, data := range datas {
		fmt.Printf("static const Byte jpeg_file_%d[] = {", i)
		for j, c := range data {
			if j%16 == 0 {
				fmt.Print("\n    ")
			}
			fmt.Printf("%#02x,", c)
		}
		fmt.Println("\n};")
	}
	fmt.Println()
	fmt.Println("static const JpegFile jpeg_files[] = {")
	for i, name := range names {
		fmt.Printf("    {%s, jpeg_file_%d, sizeof jpeg_file_%d},\n", strconv.Quote(name), i, i)
	}
	fmt.Println("};")
	fmt.Println()
	fmt.Println("static const JpegDecodeCase jpeg_decode_cases[] = {")
	seed = uint64(0x9e3779b97f4a7c15)
	skipped := 0
	for i, data := range datas {
		for j := 0; j < 40; j++ {
			s := uint64(0)
			if j > 0 {
				seed += 0x632be59bd9b4e019
				s = seed
			}
			b := mutate(data, s)
			if huge(b) {
				skipped++
				continue
			}
			res := decodeSummary(b, false) + " | " + configSummary(b, false)
			slow := decodeSummary(b, true) + " | " + configSummary(b, true)
			if slow == res {
				slow = "NULL"
			} else {
				slow = strconv.Quote(slow)
			}
			fmt.Printf("    {%d, UINT64_C(%#x), %s, %s},\n", i, s, strconv.Quote(res), slow)
		}
	}
	fmt.Println("};")
	fmt.Fprintf(os.Stderr, "skipped %d decode cases for size\n", skipped)
}
GO

cd "$tmp"
go mod init gen >/dev/null 2>&1
go run . "$testdata" > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
