package main

import _ "embed"

// Embed bytes so serving an asset does not allocate a mutable copy per request.
// These shared assets are read-only throughout the application.
//
//go:embed web/index.html
var indexHTML []byte

//go:embed web/style.css
var styleCSS []byte

//go:embed web/model.js
var modelJS []byte

//go:embed web/app.js
var appJS []byte
