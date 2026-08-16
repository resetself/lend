package lend

import (
	_ "embed"
)

//go:embed cmd/lendctl/lendctl.c
var LendctlSource []byte

//go:embed sublime/lend.py
var SublimePlugin []byte
