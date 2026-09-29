#!/bin/sh
# Regenerates tests/image_test_gen.h: what Go's image package makes of random
# points and rectangles, and of every image type after a run of random writes.
# The writes come from a small xorshift generator that tests/image_test.c runs
# too, so the header only has to carry each run's seed and what Go ended up
# with: FNV-1a digests of the pixels, of At, RGBA64At and the typed getters over
# the bounds and a pixel beyond them, of the offsets, and the same for a random
# SubImage. It also records the constructors' panics and how fmt prints Point,
# Rectangle and YCbCrSubsampleRatio. Nothing depends on the machine, so this
# runs wherever go does.
#
# Copyright 2026 The burrow Authors. All rights reserved.
# Use of this source code is governed by a BSD-style licence that can be found
# in the LICENSE file.
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/tests/image_test_gen.h"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

cat > "$tmp/main.go" <<'GO'
package main

import (
	"fmt"
	"image"
	"image/color"
	"math/rand"
	"strconv"
)

// The color kinds, in the order of the test's table of types.
const (
	kRGBA = iota
	kRGBA64
	kNRGBA
	kNRGBA64
	kAlpha
	kAlpha16
	kGray
	kGray16
	kYCbCr
	kNYCbCrA
	kCMYK
)

func fields(c color.Color) (int, [4]uint32) {
	switch v := c.(type) {
	case color.RGBA:
		return kRGBA, [4]uint32{uint32(v.R), uint32(v.G), uint32(v.B), uint32(v.A)}
	case color.RGBA64:
		return kRGBA64, [4]uint32{uint32(v.R), uint32(v.G), uint32(v.B), uint32(v.A)}
	case color.NRGBA:
		return kNRGBA, [4]uint32{uint32(v.R), uint32(v.G), uint32(v.B), uint32(v.A)}
	case color.NRGBA64:
		return kNRGBA64, [4]uint32{uint32(v.R), uint32(v.G), uint32(v.B), uint32(v.A)}
	case color.Alpha:
		return kAlpha, [4]uint32{uint32(v.A)}
	case color.Alpha16:
		return kAlpha16, [4]uint32{uint32(v.A)}
	case color.Gray:
		return kGray, [4]uint32{uint32(v.Y)}
	case color.Gray16:
		return kGray16, [4]uint32{uint32(v.Y)}
	case color.YCbCr:
		return kYCbCr, [4]uint32{uint32(v.Y), uint32(v.Cb), uint32(v.Cr)}
	case color.NYCbCrA:
		return kNYCbCrA, [4]uint32{uint32(v.Y), uint32(v.Cb), uint32(v.Cr), uint32(v.A)}
	case color.CMYK:
		return kCMYK, [4]uint32{uint32(v.C), uint32(v.M), uint32(v.Y), uint32(v.K)}
	}
	panic("unknown color")
}

// The generator the C test runs as well. Everything random about a run of
// writes comes from here, in the same order on both sides.
type xs struct{ x uint64 }

func (r *xs) next() uint64 {
	r.x ^= r.x << 13
	r.x ^= r.x >> 7
	r.x ^= r.x << 17
	return r.x
}

func (r *xs) intn(n int) int { return int(r.next() % uint64(n)) }

// A color of kind k from four draws.
func xcolor(r *xs, k int) color.Color {
	var v [4]uint64
	for i := range v {
		v[i] = r.next()
	}
	b := func(i int) uint8 { return uint8(v[i]) }
	w := func(i int) uint16 { return uint16(v[i]) }
	switch k {
	case kRGBA:
		return color.RGBA{b(0), b(1), b(2), b(3)}
	case kRGBA64:
		return color.RGBA64{w(0), w(1), w(2), w(3)}
	case kNRGBA:
		return color.NRGBA{b(0), b(1), b(2), b(3)}
	case kNRGBA64:
		return color.NRGBA64{w(0), w(1), w(2), w(3)}
	case kAlpha:
		return color.Alpha{b(0)}
	case kAlpha16:
		return color.Alpha16{w(0)}
	case kGray:
		return color.Gray{b(0)}
	case kGray16:
		return color.Gray16{w(0)}
	case kYCbCr:
		return color.YCbCr{b(0), b(1), b(2)}
	case kNYCbCrA:
		return color.NYCbCrA{color.YCbCr{b(0), b(1), b(2)}, b(3)}
	case kCMYK:
		return color.CMYK{b(0), b(1), b(2), b(3)}
	}
	panic("kind")
}

type fnv struct{ h uint64 }

func newFnv() fnv { return fnv{14695981039346656037} }

func (f *fnv) byte(b uint8) {
	f.h ^= uint64(b)
	f.h *= 1099511628211
}

func (f *fnv) u16(v uint32) {
	f.byte(uint8(v))
	f.byte(uint8(v >> 8))
}

func (f *fnv) u64(v uint64) {
	for i := 0; i < 8; i++ {
		f.byte(uint8(v >> (8 * i)))
	}
}

func (f *fnv) bytes(b []byte) {
	for _, c := range b {
		f.byte(c)
	}
}

// A color's kind, fields and RGBA, or a marker for nil.
func (f *fnv) color(c color.Color) {
	if c == nil {
		f.byte(0xee)
		return
	}
	k, v := fields(c)
	f.byte(uint8(k))
	for _, x := range v {
		f.u16(x)
	}
	r, g, b, a := c.RGBA()
	f.u16(r)
	f.u16(g)
	f.u16(b)
	f.u16(a)
}

func (f *fnv) rgba64(c color.RGBA64) {
	f.u16(uint32(c.R))
	f.u16(uint32(c.G))
	f.u16(uint32(c.B))
	f.u16(uint32(c.A))
}

func lit(s string) string { return strconv.Quote(s) }

func rect(r image.Rectangle) string {
	return fmt.Sprintf("{%d, %d, %d, %d}", r.Min.X, r.Min.Y, r.Max.X, r.Max.Y)
}

func pt(p image.Point) string { return fmt.Sprintf("{%d, %d}", p.X, p.Y) }

// The image types with Pix, Stride and Rect, in the order of the test's table.
type pixImage interface {
	image.RGBA64Image
	Set(x, y int, c color.Color)
	SetRGBA64(x, y int, c color.RGBA64)
	Opaque() bool
	PixOffset(x, y int) int
	SubImage(r image.Rectangle) image.Image
}

var palette = color.Palette{
	color.RGBA{0, 0, 0, 0xff},
	color.Gray{100},
	color.NRGBA{10, 20, 30, 128},
	color.CMYK{0x10, 0x20, 0x30, 0x40},
	color.Alpha16{0xffff},
	color.RGBA64{0x1234, 0x2345, 0x3456, 0xffff},
}

func newImage(k int, r image.Rectangle) pixImage {
	switch k {
	case 0:
		return image.NewRGBA(r)
	case 1:
		return image.NewRGBA64(r)
	case 2:
		return image.NewNRGBA(r)
	case 3:
		return image.NewNRGBA64(r)
	case 4:
		return image.NewAlpha(r)
	case 5:
		return image.NewAlpha16(r)
	case 6:
		return image.NewGray(r)
	case 7:
		return image.NewGray16(r)
	case 8:
		return image.NewCMYK(r)
	case 9:
		return image.NewPaletted(r, palette)
	}
	panic("image kind")
}

func pixOf(m image.Image) ([]byte, int) {
	switch m := m.(type) {
	case *image.RGBA:
		return m.Pix, m.Stride
	case *image.RGBA64:
		return m.Pix, m.Stride
	case *image.NRGBA:
		return m.Pix, m.Stride
	case *image.NRGBA64:
		return m.Pix, m.Stride
	case *image.Alpha:
		return m.Pix, m.Stride
	case *image.Alpha16:
		return m.Pix, m.Stride
	case *image.Gray:
		return m.Pix, m.Stride
	case *image.Gray16:
		return m.Pix, m.Stride
	case *image.CMYK:
		return m.Pix, m.Stride
	case *image.Paletted:
		return m.Pix, m.Stride
	}
	panic("image type")
}

// The typed setter each type has besides Set, with a color of its own type.
func typedSet(m pixImage, r *xs, x, y int) {
	switch m := m.(type) {
	case *image.RGBA:
		m.SetRGBA(x, y, xcolor(r, kRGBA).(color.RGBA))
	case *image.RGBA64:
		m.SetRGBA64(x, y, xcolor(r, kRGBA64).(color.RGBA64))
	case *image.NRGBA:
		m.SetNRGBA(x, y, xcolor(r, kNRGBA).(color.NRGBA))
	case *image.NRGBA64:
		m.SetNRGBA64(x, y, xcolor(r, kNRGBA64).(color.NRGBA64))
	case *image.Alpha:
		m.SetAlpha(x, y, xcolor(r, kAlpha).(color.Alpha))
	case *image.Alpha16:
		m.SetAlpha16(x, y, xcolor(r, kAlpha16).(color.Alpha16))
	case *image.Gray:
		m.SetGray(x, y, xcolor(r, kGray).(color.Gray))
	case *image.Gray16:
		m.SetGray16(x, y, xcolor(r, kGray16).(color.Gray16))
	case *image.CMYK:
		m.SetCMYK(x, y, xcolor(r, kCMYK).(color.CMYK))
	case *image.Paletted:
		m.SetColorIndex(x, y, uint8(r.intn(len(palette))))
	}
}

// The typed getter, into a digest.
func typedAt(f *fnv, m image.Image, x, y int) {
	switch m := m.(type) {
	case *image.RGBA:
		f.color(m.RGBAAt(x, y))
	case *image.RGBA64:
		f.color(m.RGBA64At(x, y))
	case *image.NRGBA:
		f.color(m.NRGBAAt(x, y))
	case *image.NRGBA64:
		f.color(m.NRGBA64At(x, y))
	case *image.Alpha:
		f.color(m.AlphaAt(x, y))
	case *image.Alpha16:
		f.color(m.Alpha16At(x, y))
	case *image.Gray:
		f.color(m.GrayAt(x, y))
	case *image.Gray16:
		f.color(m.Gray16At(x, y))
	case *image.CMYK:
		f.color(m.CMYKAt(x, y))
	case *image.Paletted:
		f.byte(m.ColorIndexAt(x, y))
	}
}

// At, RGBA64At, the typed getter and PixOffset over the bounds and one pixel
// around them.
func reads(m pixImage) uint64 {
	f := newFnv()
	b := m.Bounds()
	for y := b.Min.Y - 1; y < b.Max.Y+1; y++ {
		for x := b.Min.X - 1; x < b.Max.X+1; x++ {
			f.color(m.At(x, y))
			f.rgba64(m.RGBA64At(x, y))
			typedAt(&f, m, x, y)
			f.u64(uint64(m.PixOffset(x, y)))
		}
	}
	return f.h
}

func pixDigest(m image.Image) uint64 {
	f := newFnv()
	p, _ := pixOf(m)
	f.bytes(p)
	return f.h
}

// A rectangle somewhere around b, sometimes outside it and sometimes empty.
func subRect(r *xs, b image.Rectangle) image.Rectangle {
	x0 := b.Min.X - 2 + r.intn(b.Dx()+4)
	y0 := b.Min.Y - 2 + r.intn(b.Dy()+4)
	x1 := x0 + r.intn(b.Dx()+3)
	y1 := y0 + r.intn(b.Dy()+3)
	return image.Rectangle{image.Point{x0, y0}, image.Point{x1, y1}}
}

var rects = []image.Rectangle{
	{image.Point{0, 0}, image.Point{4, 3}},
	{image.Point{-2, -3}, image.Point{3, 1}},
	{image.Point{5, 5}, image.Point{5, 9}},
	{image.Point{2, 1}, image.Point{7, 6}},
	{image.Point{-1, -1}, image.Point{0, 0}},
}

func main() {
	rng := rand.New(rand.NewSource(2143))
	catch := func(f func()) (s string) {
		defer func() {
			if r := recover(); r != nil {
				s = fmt.Sprint(r)
			}
		}()
		f()
		return ""
	}

	fmt.Print(`/* Generated by tools/gen-image-tests.sh from Go's image. Do not edit. */

`)

	// Points against points, a factor and a rectangle.
	small := func() int { return rng.Intn(11) - 5 }
	fmt.Println("static const ImPointCase im_point_cases[] = {")
	for i := 0; i < 400; i++ {
		p := image.Point{small(), small()}
		q := image.Point{small(), small()}
		k := small()
		if k == 0 {
			k = 3
		}
		r := image.Rectangle{image.Point{small(), small()}, image.Point{small(), small()}}
		mod := image.Point{}
		hasMod := r.Dx() != 0 && r.Dy() != 0
		if hasMod {
			mod = p.Mod(r)
		}
		fmt.Printf("    {%s, %s, %d, %s, %s, %s, %s, %s, %t, %t, %s, %t, %s},\n",
			pt(p), pt(q), k, rect(r), pt(p.Add(q)), pt(p.Sub(q)), pt(p.Mul(k)), pt(p.Div(k)),
			p.In(r), hasMod, pt(mod), p.Eq(q), lit(p.String()))
	}
	fmt.Print("};\n\n")

	fmt.Println("static const ImRectCase im_rect_cases[] = {")
	for i := 0; i < 600; i++ {
		r := image.Rectangle{image.Point{small(), small()}, image.Point{small(), small()}}
		s := image.Rectangle{image.Point{small(), small()}, image.Point{small(), small()}}
		if i%5 == 0 {
			s = r
		}
		p := image.Point{small(), small()}
		n := rng.Intn(7) - 2
		c := r.At(p.X, p.Y)
		_, cv := fields(c)
		fmt.Printf("    {%s, %s, %s, %d, %s, %s, %s, %s, %s, %s, %s, %d, %d, %t, %t, %t, %t, %t, %d, %s},\n",
			rect(r), rect(s), pt(p), n, rect(r.Intersect(s)), rect(r.Union(s)), rect(r.Inset(n)),
			rect(r.Canon()), rect(r.Add(p)), rect(r.Sub(p)), pt(r.Size()), r.Dx(), r.Dy(),
			r.Empty(), r.Eq(s), r.Overlaps(s), r.In(s), p.In(r), cv[0], lit(r.String()))
	}
	fmt.Print("};\n\n")

	// Each image type through a run of writes. Mode 0 is random writes of
	// every kind, mode 1 fills the bounds with one opaque color first.
	fmt.Println("static const ImImageCase im_image_cases[] = {")
	seed := uint64(0x9e3779b97f4a7c15)
	for k := 0; k < 10; k++ {
		for ri, b := range rects {
			for mode := 0; mode < 2; mode++ {
				seed += 0x632be59bd9b4e019
				r := &xs{seed}
				m := newImage(k, b)
				fresh := reads(m)
				nops := 0
				if mode == 1 {
					for y := b.Min.Y; y < b.Max.Y; y++ {
						for x := b.Min.X; x < b.Max.X; x++ {
							m.Set(x, y, color.RGBA{0x40, 0x80, 0xc0, 0xff})
						}
					}
				} else {
					nops = 60
				}
				for i := 0; i < nops; i++ {
					op := r.intn(3)
					x := b.Min.X - 1 + r.intn(b.Dx()+2)
					y := b.Min.Y - 1 + r.intn(b.Dy()+2)
					switch op {
					case 0:
						m.Set(x, y, xcolor(r, r.intn(11)))
					case 1:
						m.SetRGBA64(x, y, xcolor(r, kRGBA64).(color.RGBA64))
					case 2:
						typedSet(m, r, x, y)
					}
				}
				pix, stride := pixOf(m)
				pd, rd, op := pixDigest(m), reads(m), m.Opaque()
				sr := subRect(r, b)
				sub := m.SubImage(sr).(pixImage)
				spix, sstride := pixOf(sub)
				sreads := reads(sub)
				sopaque := sub.Opaque()
				sb := sub.Bounds()
				if !sb.Empty() {
					sub.Set(sb.Min.X, sb.Min.Y, color.RGBA{1, 2, 3, 4})
				}
				fmt.Printf("    {%d, %d, %d, UINT64_C(0x%016x), %d, %d, UINT64_C(0x%016x), UINT64_C(0x%016x), UINT64_C(0x%016x), %t, %s, %s, %d, %d, UINT64_C(0x%016x), %t, UINT64_C(0x%016x)},\n",
					k, ri, mode, seed, len(pix), stride, fresh, pd, rd, op,
					rect(sr), rect(sb), len(spix), sstride, sreads, sopaque, pixDigest(m))
			}
		}
	}
	fmt.Print("};\n\n")

	// YCbCr and NYCbCrA over every ratio, with the planes filled from their
	// indexes.
	yrects := []image.Rectangle{
		{image.Point{0, 0}, image.Point{8, 6}},
		{image.Point{1, 1}, image.Point{9, 7}},
		{image.Point{-3, -1}, image.Point{4, 5}},
		{image.Point{0, 0}, image.Point{1, 1}},
		{image.Point{3, 2}, image.Point{3, 5}},
		{image.Point{-5, -5}, image.Point{-1, -2}},
		{image.Point{-7, 3}, image.Point{0, 4}},
	}
	fmt.Println("static const ImYCbCrCase im_y_cb_cr_cases[] = {")
	for ratio := image.YCbCrSubsampleRatio444; ratio <= image.YCbCrSubsampleRatio410; ratio++ {
		for ri, b := range yrects {
			for alpha := 0; alpha < 3; alpha++ {
				seed += 0x632be59bd9b4e019
				r := &xs{seed}
				var y *image.YCbCr
				var n *image.NYCbCrA
				if alpha == 0 {
					y = image.NewYCbCr(b, ratio)
				} else {
					n = image.NewNYCbCrA(b, ratio)
					y = &n.YCbCr
					for i := range n.A {
						if alpha == 1 {
							n.A[i] = byte(i*17 + 7)
						} else {
							n.A[i] = 0xff
						}
					}
				}
				for i := range y.Y {
					y.Y[i] = byte(i*7 + 1)
				}
				for i := range y.Cb {
					y.Cb[i] = byte(i*11 + 3)
				}
				for i := range y.Cr {
					y.Cr[i] = byte(i*13 + 5)
				}
				var m interface {
					image.RGBA64Image
					Opaque() bool
					SubImage(image.Rectangle) image.Image
				} = y
				if n != nil {
					m = n
				}
				// Reading can panic: COffset rounds toward zero, so for a
				// rectangle left of or above the origin it can point past the
				// end of Cb and Cr. The digest is of what came before.
				yreads := func(m image.Image) (h uint64, msg string) {
					f := newFnv()
					defer func() {
						if r := recover(); r != nil {
							h, msg = f.h, fmt.Sprint(r)
						}
					}()
					bb := m.Bounds()
					var yy *image.YCbCr
					var nn *image.NYCbCrA
					switch v := m.(type) {
					case *image.YCbCr:
						yy = v
					case *image.NYCbCrA:
						nn = v
						yy = &v.YCbCr
					}
					for py := bb.Min.Y - 1; py < bb.Max.Y+1; py++ {
						for px := bb.Min.X - 1; px < bb.Max.X+1; px++ {
							f.color(m.At(px, py))
							f.rgba64(m.(image.RGBA64Image).RGBA64At(px, py))
							f.color(yy.YCbCrAt(px, py))
							f.u64(uint64(yy.YOffset(px, py)))
							f.u64(uint64(yy.COffset(px, py)))
							if nn != nil {
								f.color(nn.NYCbCrAAt(px, py))
								f.u64(uint64(nn.AOffset(px, py)))
							}
						}
					}
					return f.h, ""
				}
				sr := subRect(r, b)
				var sub image.Image
				subPanic := catch(func() { sub = m.SubImage(sr) })
				if sub == nil {
					sub = image.NewYCbCr(image.Rectangle{}, ratio)
				}
				var sy *image.YCbCr
				var slen [4]int
				sastride := 0
				switch v := sub.(type) {
				case *image.YCbCr:
					sy = v
				case *image.NYCbCrA:
					sy = &v.YCbCr
					slen[3] = len(v.A)
					sastride = v.AStride
				}
				slen[0], slen[1], slen[2] = len(sy.Y), len(sy.Cb), len(sy.Cr)
				var lens [4]int
				var caps [4]int
				astride := 0
				lens[0], lens[1], lens[2] = len(y.Y), len(y.Cb), len(y.Cr)
				caps[0], caps[1], caps[2] = cap(y.Y), cap(y.Cb), cap(y.Cr)
				if n != nil {
					lens[3], caps[3], astride = len(n.A), cap(n.A), n.AStride
				}
				rd, rp := yreads(m)
				srd, srp := yreads(sub)
				fmt.Printf("    {%d, %d, %d, UINT64_C(0x%016x), {%d, %d, %d, %d}, {%d, %d, %d, %d}, %d, %d, %d, UINT64_C(0x%016x), %s, %t, %s, %s, %s, {%d, %d, %d, %d}, %d, %d, %d, UINT64_C(0x%016x), %s, %t},\n",
					int(ratio), ri, alpha, seed, lens[0], lens[1], lens[2], lens[3], caps[0], caps[1], caps[2], caps[3],
					y.YStride, y.CStride, astride, rd, lit(rp), m.Opaque(), rect(sr), lit(subPanic), rect(sub.Bounds()),
					slen[0], slen[1], slen[2], slen[3], sy.YStride, sy.CStride, sastride, srd, lit(srp),
					sub.(interface{ Opaque() bool }).Opaque())
			}
		}
	}
	fmt.Print("};\n\n")

	// What the constructors panic with.
	neg := image.Rectangle{image.Point{0, 0}, image.Point{-1, 5}}
	huge := image.Rectangle{image.Point{0, 0}, image.Point{1 << 40, 1 << 40}}
	ctors := []func(image.Rectangle){
		func(r image.Rectangle) { image.NewRGBA(r) },
		func(r image.Rectangle) { image.NewRGBA64(r) },
		func(r image.Rectangle) { image.NewNRGBA(r) },
		func(r image.Rectangle) { image.NewNRGBA64(r) },
		func(r image.Rectangle) { image.NewAlpha(r) },
		func(r image.Rectangle) { image.NewAlpha16(r) },
		func(r image.Rectangle) { image.NewGray(r) },
		func(r image.Rectangle) { image.NewGray16(r) },
		func(r image.Rectangle) { image.NewCMYK(r) },
		func(r image.Rectangle) { image.NewPaletted(r, palette) },
		func(r image.Rectangle) { image.NewYCbCr(r, image.YCbCrSubsampleRatio420) },
		func(r image.Rectangle) { image.NewNYCbCrA(r, image.YCbCrSubsampleRatio420) },
	}
	fmt.Println("static const char *const im_ctor_panics[][2] = {")
	for _, c := range ctors {
		fmt.Printf("    {%s, %s},\n", lit(catch(func() { c(neg) })), lit(catch(func() { c(huge) })))
	}
	fmt.Print("};\n\n")

	// How fmt prints the types with a String method.
	fmt.Println("static const ImFmtCase im_fmt_cases[] = {")
	vals := []any{
		image.Point{1, -2},
		image.Rectangle{image.Point{-3, 4}, image.Point{5, -6}},
		image.YCbCrSubsampleRatio420,
		image.YCbCrSubsampleRatio(9),
	}
	for i, v := range vals {
		fmt.Printf("    {%d, %s, %s, %s, %s},\n", i, lit(fmt.Sprintf("%v", v)), lit(fmt.Sprintf("%+v", v)),
			lit(fmt.Sprintf("%#v", v)), lit(fmt.Sprintf("%d", v)))
	}
	fmt.Print("};\n")
}
GO

(cd "$tmp" && go mod init gen >/dev/null 2>&1 && go run . > "$out.tmp")
mv "$out.tmp" "$out"
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$out"
fi
