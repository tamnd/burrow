// Package inventory is the fixture for burrow-gen from-go. It is never built
// with Go, only read, and tests/fromgo/inventory_gen.c and inventory_gen.h are
// what tools/from-go.c writes for it. tools/check-gen.sh keeps them current.
package inventory

import "time"

// Kind says what an item is.
type Kind int

const (
	Tool Kind = iota
	Part
	Spare
	kindCount
)

// MaxItems is how many items a shelf holds.
const MaxItems = 64

// Name is a string constant with escapes in it.
const Name = "inv\t\"store\"??/"

const (
	Ratio       = 0.75
	Big  uint64 = 1 << 63
	Debug       = false
)

// Label is a named string.
type Label string

// Item is one thing on a shelf.
type Item struct {
	ID      int64  `json:"id"`
	Name    string `json:"name,omitempty"`
	Kind    Kind
	Tags    []string
	Counts  map[string]int
	Code    [4]byte
	Next    *Item
	Added   time.Time
	Life    time.Duration
	Raw     []byte
	Extra   any
	weight  float64
	_       int32
	Default bool
}

// Shelf holds items.
type Shelf struct {
	Label
	Items   []*Item
	Grid    [2][3]int
	ByKind  map[Kind][]Item
	LastErr error
}

type (
	// Shelves is every shelf.
	Shelves []Shelf
	Index   map[string]*Shelf
	Slot    [8]uint32
)

// ItemAlias is another name for Item.
type ItemAlias = Item

// NewShelf makes an empty shelf.
func NewShelf(label Label) *Shelf { return &Shelf{Label: label} }

// Add puts an item on the shelf.
func (s *Shelf) Add(it Item) {}

// Find looks an item up by its ID.
func (s *Shelf) Find(id int64) (Item, bool) { return Item{}, false }

// Len is how many items there are.
func (s Shelf) Len() int { return len(s.Items) }

// Total adds the counts up.
func Total(shelves ...Shelf) (n int, err error) { return 0, nil }

// Parse reads shelves.
func Parse(data []byte) (Shelves, error) { return nil, nil }

// Reset empties the shelf.
func (s *Shelf) Reset() {}

func helper() {}
