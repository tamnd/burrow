#!/bin/sh
# Regenerates tests/image_draw_test_gen.h: what Go's image/draw does to random
# images. Each case is a seed. The generator and tests/image_draw_test.c both
# use it to pick a destination, a source and a mask from every image type,
# fill their pixels with random bytes, and pick a rectangle, points, an op and
# which of Draw, DrawMask, Op.Draw and FloydSteinberg to call. The header
# carries the seed and an FNV-1a digest of the destination's pixels after the
# call, or the panic the call ended in. Nothing depends on the machine, so this
# runs wherever go does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/image_draw_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"fmt"
	"image"
	"image/color"
	"image/color/palette"
	"image/draw"
	"strconv"
)

type xs struct{ x uint64 }

func (r *xs) next() uint64 {
	r.x ^= r.x << 13
	r.x ^= r.x >> 7
	r.x ^= r.x << 17
	return r.x
}

func (r *xs) intn(n int) int { return int(r.next() % uint64(n)) }

// A color of kind k from four draws, the same as tests/image_test.c's.
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

// An image with only the three methods of image.Image and Set, so that
// DrawMask takes its slowest path.
type plain struct{ m *image.NRGBA }

func (p plain) ColorModel() color.Model       { return p.m.ColorModel() }
func (p plain) Bounds() image.Rectangle        { return p.m.Bounds() }
func (p plain) At(x, y int) color.Color        { return p.m.At(x, y) }
func (p plain) Set(x, y int, c color.Color)    { p.m.Set(x, y, c) }

// A mask with only the three methods of image.Image.
type plainMask struct{ m *image.Alpha }

func (p plainMask) ColorModel() color.Model { return p.m.ColorModel() }
func (p plainMask) Bounds() image.Rectangle { return p.m.Bounds() }
func (p plainMask) At(x, y int) color.Color { return p.m.At(x, y) }

// The image kinds. The ones before kYCbCr have Set and can be a destination.
const (
	kRGBA = iota
	kRGBA64
	kNRGBA
	kNRGBA64
	kAlpha
	kAlpha16
	kGray
	kGray16
	kCMYK
	kPaletted
	kPlain
	kYCbCr
	kNYCbCrA
	kUniform
	kPlainMask
	kNil
)

type built struct {
	img  image.Image
	pix  func() []byte
}

func xrect(r *xs) image.Rectangle {
	x0, y0 := r.intn(9)-4, r.intn(9)-4
	return image.Rect(x0, y0, x0+r.intn(12), y0+r.intn(12))
}

func fill(r *xs, b []byte) {
	for i := range b {
		b[i] = byte(r.next())
	}
}

func xpalette(r *xs) color.Palette {
	switch r.intn(4) {
	case 0:
		return palette.Plan9
	case 1:
		return palette.WebSafe
	}
	p := make(color.Palette, 1+r.intn(16))
	for i := range p {
		p[i] = xcolor(r, r.intn(11))
	}
	return p
}

func build(r *xs, k int) built {
	rect := xrect(r)
	switch k {
	case kRGBA:
		m := image.NewRGBA(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kRGBA64:
		m := image.NewRGBA64(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kNRGBA:
		m := image.NewNRGBA(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kNRGBA64:
		m := image.NewNRGBA64(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kAlpha:
		m := image.NewAlpha(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kAlpha16:
		m := image.NewAlpha16(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kGray:
		m := image.NewGray(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kGray16:
		m := image.NewGray16(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kCMYK:
		m := image.NewCMYK(rect)
		fill(r, m.Pix)
		return built{m, func() []byte { return m.Pix }}
	case kPaletted:
		m := image.NewPaletted(rect, xpalette(r))
		for i := range m.Pix {
			m.Pix[i] = byte(r.next() % uint64(len(m.Palette)))
		}
		return built{m, func() []byte { return m.Pix }}
	case kPlain:
		m := image.NewNRGBA(rect)
		fill(r, m.Pix)
		return built{plain{m}, func() []byte { return m.Pix }}
	case kYCbCr:
		m := image.NewYCbCr(rect, image.YCbCrSubsampleRatio(r.intn(6)))
		fill(r, m.Y)
		fill(r, m.Cb)
		fill(r, m.Cr)
		return built{m, nil}
	case kNYCbCrA:
		m := image.NewNYCbCrA(rect, image.YCbCrSubsampleRatio(r.intn(6)))
		fill(r, m.Y)
		fill(r, m.Cb)
		fill(r, m.Cr)
		fill(r, m.A)
		return built{m, nil}
	case kUniform:
		return built{image.NewUniform(xcolor(r, r.intn(11))), nil}
	case kPlainMask:
		m := image.NewAlpha(rect)
		fill(r, m.Pix)
		return built{plainMask{m}, nil}
	}
	return built{nil, nil}
}

// Masks: nil, Alpha, Alpha16, Uniform, RGBA, Gray and plainMask.
var maskKinds = []int{kNil, kAlpha, kAlpha16, kUniform, kRGBA, kGray, kPlainMask}

func digest(b []byte) uint64 {
	h := uint64(14695981039346656037)
	for _, c := range b {
		h ^= uint64(c)
		h *= 1099511628211
	}
	return h
}

func run(seed uint64) (d uint64, msg string) {
	r := &xs{seed}
	dk := r.intn(kYCbCr)
	if r.intn(2) == 0 {
		dk = kRGBA
	}
	sk := r.intn(kPlainMask)
	if r.intn(3) == 0 {
		sk = dk
	}
	mk := maskKinds[r.intn(len(maskKinds))]
	if r.intn(3) == 0 {
		mk = kNil
	}
	op := draw.Op(r.intn(2))
	how := r.intn(8)
	dst := build(r, dk)
	src := dst
	if r.intn(4) != 0 {
		src = build(r, sk)
	}
	var mask image.Image
	if mk != kNil {
		mask = build(r, mk).img
	}
	rect := xrect(r)
	sp := image.Pt(r.intn(20)-8, r.intn(20)-8)
	mp := image.Pt(r.intn(20)-8, r.intn(20)-8)
	defer func() {
		if e := recover(); e != nil {
			msg = fmt.Sprint(e)
		}
	}()
	di := dst.img.(draw.Image)
	switch how {
	case 0:
		draw.FloydSteinberg.Draw(di, rect, src.img, sp)
	case 1:
		op.Draw(di, rect, src.img, sp)
	case 2:
		draw.Draw(di, rect, src.img, sp, op)
	default:
		draw.DrawMask(di, rect, src.img, sp, mask, mp, op)
	}
	return digest(dst.pix()), ""
}

func main() {
	fmt.Println("/* Generated by tools/gen-image-draw-tests.sh from Go's image/draw. Do not")
	fmt.Println(" * edit. */")
	fmt.Println()
	fmt.Println("static const DrawCase draw_cases[] = {")
	seed := uint64(0x2545f4914f6cdd1d)
	for i := 0; i < 10000; i++ {
		seed += 0x632be59bd9b4e019
		d, msg := run(seed)
		m := "NULL"
		if msg != "" {
			m = strconv.Quote(msg)
		}
		fmt.Printf("    {UINT64_C(%#x), UINT64_C(%#x), %s},\n", seed, d, m)
	}
	fmt.Println("};")
}
GO

cd "$tmp"
go mod init gen >/dev/null 2>&1
go run . > "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
