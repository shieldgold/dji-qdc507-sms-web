package main

import _ "embed"

//go:embed web/index.html
var indexHTML string

//go:embed web/style.css
var styleCSS string

//go:embed web/model.js
var modelJS string

//go:embed web/app.js
var appJS string
