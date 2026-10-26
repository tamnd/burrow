// The Go side of http.c.
package main

import (
	"fmt"
	"net/http"
)

func main() {
	http.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "hello\n")
	})
	err := http.ListenAndServe(":8080", nil)
	fmt.Println(err)
}
