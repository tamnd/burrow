// Package weather is the Go that docs/examples/reflect/weather_gen.h was
// written from, with burrow-gen from-go weather.go -o weather_gen.h.
package weather

// Celsius is a temperature.
type Celsius float64

// Reading is what one sensor saw.
type Reading struct {
	Sensor string   `json:"sensor"`
	Temp   Celsius  `json:"temp"`
	Tags   []string `json:"tags,omitempty"`
}

// Freezing is where water freezes.
const Freezing Celsius = 0

// Below reports whether the reading is colder than t.
func (r Reading) Below(t Celsius) bool { return r.Temp < t }
