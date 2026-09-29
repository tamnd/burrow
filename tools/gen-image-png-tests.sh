#!/bin/sh
# Regenerates tests/image_png_test_gen.h: what Go's image/png makes of random
# images and of damaged files. The header has two tables. The first is seeds:
# the generator and tests/image_png_test.c both use each one to build an image
# of a random type, size and content, and the case holds a digest of the PNG
# Go writes at each compression level, then what Go's Decode and DecodeConfig
# make of the default one. The second is the PNG files from Go's testdata,
# PngSuite and the invalid ones, each with seeds that flip bytes, fix up the
# checksums or cut the file short, and what Decode and DecodeConfig say about
# the result, and what they say when the file comes one byte a read where
# that is different. Nothing depends on the machine, so this runs wherever go does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/image_png_test_gen.h"
testdata="$(go env GOROOT)/src/image/png/testdata"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"hash/crc32"
	"image"
	"image/color"
	"image/png"
	"io"
	"os"
	"path/filepath"
	"sort"
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

// build: an image of kind k, as tests/image_png_test.c builds it.
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

func decodeSummary(data []byte, slow bool) string {
	m, err := png.Decode(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	b := m.Bounds()
	size := fmt.Sprintf("%dx%d", b.Dx(), b.Dy())
	switch p := m.(type) {
	case *image.Gray:
		return fmt.Sprintf("Gray %s %016x", size, fnv(p.Pix))
	case *image.Gray16:
		return fmt.Sprintf("Gray16 %s %016x", size, fnv(p.Pix))
	case *image.RGBA:
		return fmt.Sprintf("RGBA %s %016x", size, fnv(p.Pix))
	case *image.RGBA64:
		return fmt.Sprintf("RGBA64 %s %016x", size, fnv(p.Pix))
	case *image.NRGBA:
		return fmt.Sprintf("NRGBA %s %016x", size, fnv(p.Pix))
	case *image.NRGBA64:
		return fmt.Sprintf("NRGBA64 %s %016x", size, fnv(p.Pix))
	case *image.Paletted:
		return fmt.Sprintf("Paletted %s %016x %s", size, fnv(p.Pix), paletteDigest(p.Palette))
	}
	return "?"
}

func configSummary(data []byte, slow bool) string {
	c, err := png.DecodeConfig(reader(data, slow))
	if err != nil {
		return "E:" + err.Error()
	}
	var model string
	switch c.ColorModel {
	case color.GrayModel:
		model = "gray"
	case color.Gray16Model:
		model = "gray16"
	case color.RGBAModel:
		model = "rgba"
	case color.RGBA64Model:
		model = "rgba64"
	case color.NRGBAModel:
		model = "nrgba"
	case color.NRGBA64Model:
		model = "nrgba64"
	default:
		model = paletteDigest(c.ColorModel.(color.Palette))
	}
	return fmt.Sprintf("%dx%d %s", c.Width, c.Height, model)
}

var levels = []png.CompressionLevel{png.DefaultCompression, png.NoCompression,
	png.BestSpeed, png.BestCompression, 7}

func encodeCase(seed uint64) string {
	var parts []string
	var first []byte
	for i, l := range levels {
		r := &xs{seed}
		m := build(r)
		var buf bytes.Buffer
		enc := png.Encoder{CompressionLevel: l}
		if err := enc.Encode(&buf, m); err != nil {
			parts = append(parts, "E:"+err.Error())
			continue
		}
		if i == 0 {
			first = buf.Bytes()
		}
		parts = append(parts, fmt.Sprintf("%d:%016x", buf.Len(), fnv(buf.Bytes())))
	}
	s := strings.Join(parts, " ")
	if first != nil {
		s += " | " + decodeSummary(first, false) + " | " + configSummary(first, false)
	}
	return s
}

// fixCRC recomputes the checksum of every whole chunk, so that a flipped
// byte gets past the CRC check to the code behind it.
func fixCRC(b []byte) {
	off := 8
	for off+12 <= len(b) {
		l := uint64(binary.BigEndian.Uint32(b[off:]))
		if l > uint64(len(b)-off-12) {
			break
		}
		end := off + 8 + int(l)
		binary.BigEndian.PutUint32(b[end:], crc32.ChecksumIEEE(b[off+4:end]))
		off = end + 4
	}
}

func mutate(data []byte, seed uint64) []byte {
	b := append([]byte(nil), data...)
	if seed == 0 {
		return b
	}
	r := &xs{seed}
	op := r.intn(3)
	if op == 2 {
		return b[:r.intn(len(b))]
	}
	n := 1 + r.intn(3)
	for i := 0; i < n; i++ {
		pos := 8 + r.intn(len(b)-8)
		b[pos] ^= byte(1 + r.intn(255))
	}
	if op == 0 {
		fixCRC(b)
	}
	return b
}

func main() {
	dir := os.Args[1]
	var files []string
	suite, _ := filepath.Glob(filepath.Join(dir, "pngsuite", "*.png"))
	sort.Strings(suite)
	files = append(files, suite...)
	for _, name := range []string{"gray-gradient.png", "gray-gradient.interlaced.png",
		"invalid-crc32.png", "invalid-noend.png", "invalid-palette.png",
		"invalid-trunc.png", "invalid-zlib.png"} {
		files = append(files, filepath.Join(dir, name))
	}

	fmt.Println("/* Generated by tools/gen-image-png-tests.sh from Go's image/png and the")
	fmt.Println(" * PNG files in its testdata. Do not edit. */")
	fmt.Println()
	fmt.Println("static const PngEncodeCase png_encode_cases[] = {")
	seed := uint64(0x2545f4914f6cdd1d)
	for i := 0; i < 3000; i++ {
		seed += 0x632be59bd9b4e019
		fmt.Printf("    {UINT64_C(%#x), %s},\n", seed, strconv.Quote(encodeCase(seed)))
	}
	fmt.Println("};")
	fmt.Println()

	var datas [][]byte
	for i, f := range files {
		data, err := os.ReadFile(f)
		if err != nil {
			panic(err)
		}
		datas = append(datas, data)
		fmt.Printf("static const Byte png_file_%d[] = {", i)
		for j, c := range data {
			if j%16 == 0 {
				fmt.Print("\n    ")
			}
			fmt.Printf("%#02x,", c)
		}
		fmt.Println("\n};")
	}
	fmt.Println()
	fmt.Println("static const PngFile png_files[] = {")
	for i, f := range files {
		name := strings.TrimPrefix(f, dir+"/")
		fmt.Printf("    {%s, png_file_%d, sizeof png_file_%d},\n", strconv.Quote(name), i, i)
	}
	fmt.Println("};")
	fmt.Println()
	fmt.Println("static const PngDecodeCase png_decode_cases[] = {")
	seed = uint64(0x9e3779b97f4a7c15)
	for i, data := range datas {
		for j := 0; j < 25; j++ {
			s := uint64(0)
			if j > 0 {
				seed += 0x632be59bd9b4e019
				s = seed
			}
			b := mutate(data, s)
			if c, err := png.DecodeConfig(bytes.NewReader(b)); err == nil &&
				int64(c.Width)*int64(c.Height) > 1<<24 {
				// A flipped size byte that asks for gigabytes. Go and a
				// plain C build get the memory lazily from the kernel,
				// but a sanitizer build or a strict overcommit policy
				// does not, so leave these out.
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
}
GO

cd "$tmp"
go mod init gen >/dev/null 2>&1
go run . "$testdata" > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
