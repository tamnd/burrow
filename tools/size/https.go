// The Go side of https.c.
package main

import (
	"fmt"
	"net/http"
)

func main() {
	http.HandleFunc("GET /", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprintf(w, "hello\n")
	})
	err := http.ListenAndServeTLS(":8443", "cert.pem", "key.pem", nil)
	fmt.Println(err)
}
