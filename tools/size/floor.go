// The Go side of floor.c. Go has no way to leave its runtime out, so this is
// as small as the same program gets.
package main

import (
	"strconv"
	"strings"
	"time"
)

func main() {
	f, _ := strconv.ParseFloat("3.25", 64)
	up := strings.ToUpper("hello")
	t, _ := time.Parse(time.RFC3339, "2026-03-08T14:30:00-05:00")
	println(f, up, t.Format(time.Kitchen))
}
