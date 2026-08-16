package lend

import (
	_ "embed"
)

//go:embed cmd/lendctl/lendctl.c
var LendctlSource []byte
