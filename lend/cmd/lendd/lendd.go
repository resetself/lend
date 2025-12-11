package main

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"lend"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"syscall"
)

const port = 52698

var baseMountPath string

func init() {
	home, err := os.UserHomeDir()
	if err != nil {
		log.Fatalf("Failed to get home directory: %v", err)
	}
	baseMountPath = filepath.Join(home, ".lend", "files")
}

func main() {
	cfg := &net.ListenConfig{
		Control: func(_, _ string, c syscall.RawConn) error {
			return c.Control(func(fd uintptr) {
				syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, syscall.SO_REUSEADDR, 1)
				_ = syscall.SetsockoptInt(int(fd), syscall.SOL_SOCKET, 0x0F, 1)
			})
		},
	}

	ln, err := cfg.Listen(context.Background(), "tcp", fmt.Sprintf(":%d", port))
	if err != nil {
		log.Fatalf("Failed to listen on port: %v", err)
	}
	defer ln.Close()

	log.Printf("Listening on port %d", port)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("Connection failed: %v", err)
			continue
		}
		go handle(conn)
	}
}

func handle(conn net.Conn) {
	defer conn.Close()

	data, err := bufio.NewReader(conn).ReadBytes('\n')
	if err != nil {
		log.Printf("Failed to read data: %v", err)
		return
	}

	parts := strings.Fields(strings.TrimSpace(string(data)))
	if len(parts) < 1 {
		return
	}

	command := parts[0]
	if command == "install_lendctl" {
		lendctlInstall(conn)
		return
	}

	// Build args list
	var args []string
	for _, part := range parts[1:] {
		value := strings.SplitN(part, "|", 2)
		if len(value) == 2 && value[0] == "FILE" {
			// File argument includes host path, e.g. FILE|server1/config.yaml
			args = append(args, filepath.Join(baseMountPath, value[1]))
		} else {
			args = append(args, value[0])
		}
	}

	execCmd(conn, command, args)
}

func lendctlInstall(conn net.Conn) {
	conn.Write(lend.LendctlSource)
	log.Printf("Sent lendctl.c (%d bytes)", len(lend.LendctlSource))
}

func execCmd(conn net.Conn, command string, args []string) {
	var out, stderr bytes.Buffer
	c := exec.Command(command, args...)
	c.Stdout = &out
	c.Stderr = &stderr

	if err := c.Run(); err != nil {
		errMsg := stderr.String()
		if errMsg == "" {
			errMsg = err.Error()
		}
		conn.Write([]byte("ERROR|" + errMsg + "\n"))
		log.Printf("Command failed [%s %v]: %v", command, args, err)
	} else {
		conn.Write([]byte("OK|" + out.String() + "\n"))
	}
}
