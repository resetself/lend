package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"fmt"
	"io"
	"lend"
	"log"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

const maxBlobSize = 1 << 30 // 1GB guard against malformed length fields

// waitFlags maps fork-and-return GUI editors to the flag that makes them block
// until the edited file/window is closed. Without it, the editor exits
// immediately, lendd pushes back unchanged content, and the temp workdir is
// removed while the user is still editing (so edits never sync and a directory
// opened in the editor can't read its files).
var waitFlags = map[string][]string{
	"subl":          {"-w"},
	"sublime":       {"-w"},
	"sublime_text":  {"-w"},
	"code":          {"-w"},
	"code-insiders": {"-w"},
	"atom":          {"-w"},
	"gedit":         {"-w"},
	"kate":          {"--block"},
}

// dirBlockers lists editors whose wait flag already blocks for a directory
// window (so they need no sentinel file). Editors not listed here (notably
// Sublime Text, where `subl -w <dir>` returns immediately) get an empty
// .lend-wait sentinel opened alongside the folder so that closing the window
// also unblocks the wait flag.
var dirBlockers = map[string]bool{
	"code":          true,
	"code-insiders": true,
}

var (
	baseDir    string
	socketPath string
)

func init() {
	home, err := os.UserHomeDir()
	if err != nil {
		log.Fatalf("Failed to get home directory: %v", err)
	}
	baseDir = filepath.Join(home, ".lend")
	socketPath = filepath.Join(baseDir, "lendd.sock")
}

func main() {
	if err := os.MkdirAll(filepath.Join(baseDir, "files"), 0700); err != nil {
		log.Fatalf("Failed to create files dir: %v", err)
	}
	if err := os.Chmod(baseDir, 0700); err != nil {
		log.Printf("chmod base dir: %v", err)
	}

	// Remove stale socket from a previous run
	_ = os.Remove(socketPath)

	ln, err := net.Listen("unix", socketPath)
	if err != nil {
		log.Fatalf("Failed to listen on %s: %v", socketPath, err)
	}
	defer ln.Close()
	_ = os.Chmod(socketPath, 0600)

	log.Printf("Listening on %s", socketPath)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("Accept failed: %v", err)
			continue
		}
		go handle(conn)
	}
}

// arg is one parsed request argument.
type arg struct {
	kind    string // "S" string, "F" file, "D" directory, "O" output
	name    string // basename (F/D/O)
	data    []byte // S: string bytes; F: file content; D: tree blob
	matPath string // materialized path in the temp workdir (F/D/O)
}

func handle(conn net.Conn) {
	defer conn.Close()

	br := bufio.NewReader(conn)

	line, err := readLine(br)
	if err != nil {
		log.Printf("read RUN line: %v", err)
		return
	}

	// Zero-install bootstrap: the remote sends "install_lendctl" and we reply
	// with the embedded lendctl.c source, which it compiles with its local gcc.
	if line == "install_lendctl" {
		lendctlInstall(conn)
		return
	}

	fields := strings.Fields(line)
	if len(fields) < 3 || fields[0] != "RUN" {
		log.Printf("bad RUN line: %q", line)
		return
	}
	tool := fields[1]
	nargs, err := strconv.Atoi(fields[2])
	if err != nil || nargs < 0 || nargs > 100000 {
		log.Printf("bad nargs: %q", fields[2])
		return
	}

	args := make([]arg, 0, nargs)
	for i := 0; i < nargs; i++ {
		hdr, err := readLine(br)
		if err != nil {
			log.Printf("read arg header: %v", err)
			return
		}
		parts := strings.Fields(hdr)
		if len(parts) < 2 {
			log.Printf("bad arg header: %q", hdr)
			return
		}
		switch parts[0] {
		case "S":
			n := atoi(parts[1])
			data, err := readN(br, n)
			if err != nil {
				log.Printf("read S payload: %v", err)
				return
			}
			args = append(args, arg{kind: "S", data: data})
		case "F":
			nameLen, contentLen := atoi(parts[1]), atoi(parts[2])
			name, err := readN(br, nameLen)
			if err != nil {
				log.Printf("read F name: %v", err)
				return
			}
			content, err := readN(br, contentLen)
			if err != nil {
				log.Printf("read F content: %v", err)
				return
			}
			args = append(args, arg{kind: "F", name: string(name), data: content})
		case "D":
			nameLen, blobLen := atoi(parts[1]), atoi(parts[2])
			name, err := readN(br, nameLen)
			if err != nil {
				log.Printf("read D name: %v", err)
				return
			}
			blob, err := readN(br, blobLen)
			if err != nil {
				log.Printf("read D blob: %v", err)
				return
			}
			args = append(args, arg{kind: "D", name: string(name), data: blob})
		case "O":
			nameLen := atoi(parts[1])
			name, err := readN(br, nameLen)
			if err != nil {
				log.Printf("read O name: %v", err)
				return
			}
			args = append(args, arg{kind: "O", name: string(name)})
		default:
			log.Printf("unknown arg kind: %q", parts[0])
			return
		}
	}

	if _, ok := waitFlags[filepath.Base(tool)]; ok {
		runEdit(conn, tool, args)
	} else {
		run(conn, tool, args)
	}
}

func lendctlInstall(conn net.Conn) {
	_, _ = conn.Write(lend.LendctlSource)
	log.Printf("Sent lendctl.c (%d bytes)", len(lend.LendctlSource))
}

func run(conn net.Conn, tool string, args []arg) {
	workdir, err := os.MkdirTemp(filepath.Join(baseDir, "files"), "run-")
	if err != nil {
		log.Printf("MkdirTemp: %v", err)
		return
	}
	defer os.RemoveAll(workdir)

	cmdArgs, ok := materialize(workdir, args)
	if !ok {
		return
	}

	cmd := exec.Command(tool, cmdArgs...)
	cmd.Dir = workdir
	var stdout, stderr bytes.Buffer
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr
	err = cmd.Run()

	exitCode := 0
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			exitCode = ee.ExitCode()
		} else {
			// command not found or failed to start
			exitCode = 127
			if stderr.Len() == 0 {
				stderr.WriteString(err.Error())
			}
		}
	}

	w := bufio.NewWriter(conn)
	fmt.Fprintf(w, "RESULT %d\n", exitCode)
	writeFrame(w, "SO", stdout.Bytes())
	writeFrame(w, "SE", stderr.Bytes())

	for _, a := range args {
		switch a.kind {
		case "S":
			// no response frame for plain strings
		case "F":
			data, err := os.ReadFile(a.matPath)
			if err != nil {
				writeFrame(w, "RF", nil)
			} else {
				writeFrame(w, "RF", data)
			}
		case "D":
			blob, err := writeTree(a.matPath)
			if err != nil {
				writeFrame(w, "RD", nil)
			} else {
				writeFrame(w, "RD", blob)
			}
		case "O":
			info, err := os.Stat(a.matPath)
			if err != nil || !info.Mode().IsRegular() {
				writeFrame(w, "RO", nil)
				continue
			}
			data, err := os.ReadFile(a.matPath)
			if err != nil {
				writeFrame(w, "RO", nil)
			} else {
				writeFrame(w, "RO", data)
			}
		}
	}

	if err := w.Flush(); err != nil {
		log.Printf("flush response: %v", err)
	}
}

// materialize writes F/D/O args into the workdir and returns the exec args,
// populating each arg's matPath. ok is false if any materialization failed.
func materialize(workdir string, args []arg) (cmdArgs []string, ok bool) {
	cmdArgs = make([]string, 0, len(args))
	used := map[string]int{}
	for i := range args {
		a := &args[i]
		switch a.kind {
		case "S":
			cmdArgs = append(cmdArgs, string(a.data))
		case "F":
			name := dedup(used, safeName(a.name))
			dst := filepath.Join(workdir, name)
			if err := os.WriteFile(dst, a.data, 0644); err != nil {
				log.Printf("write file %s: %v", dst, err)
				return nil, false
			}
			a.matPath = dst
			cmdArgs = append(cmdArgs, dst)
		case "D":
			name := dedup(used, safeName(a.name))
			dst := filepath.Join(workdir, name)
			if err := os.MkdirAll(dst, 0755); err != nil {
				log.Printf("mkdir %s: %v", dst, err)
				return nil, false
			}
			if err := readTree(a.data, dst); err != nil {
				log.Printf("extract tree %s: %v", dst, err)
				return nil, false
			}
			a.matPath = dst
			cmdArgs = append(cmdArgs, dst)
		case "O":
			// Output file: materialize the path but do not create it, so we can
			// detect whether the tool actually produced it.
			name := dedup(used, safeName(a.name))
			dst := filepath.Join(workdir, name)
			a.matPath = dst
			cmdArgs = append(cmdArgs, dst)
		}
	}
	return cmdArgs, true
}

func writeUpdate(w *bufio.Writer, idx int, data []byte) {
	fmt.Fprintf(w, "UPDATE %d %d\n", idx, len(data))
	_, _ = w.Write(data)
}

func writeTreeFrame(w *bufio.Writer, idx int, blob []byte) {
	fmt.Fprintf(w, "TREE %d %d\n", idx, len(blob))
	_, _ = w.Write(blob)
}

// runEdit handles GUI editors as a live-sync session: the editor is started in
// the background (with its wait flag), and lendd streams every save back to the
// client immediately, ending the session (and removing the temp files) when the
// editor closes the file/window.
func runEdit(conn net.Conn, tool string, args []arg) {
	workdir, err := os.MkdirTemp(filepath.Join(baseDir, "files"), "run-")
	if err != nil {
		log.Printf("MkdirTemp: %v", err)
		return
	}
	defer os.RemoveAll(workdir)

	cmdArgs, ok := materialize(workdir, args)
	if !ok {
		return
	}

	hasDir := false
	for i := range args {
		if args[i].kind == "D" {
			hasDir = true
			break
		}
	}

	// Some editors' wait flag only blocks for files, not a directory window
	// (Sublime Text: `subl -w dir` returns immediately while the window stays
	// open). To get a reliable "window closed" signal we open an empty sentinel
	// file alongside the folder; when the window closes, the sentinel closes
	// and the editor's -w unblocks. Editors whose wait flag already blocks for
	// folders (code/code-insiders) don't need it.
	if hasDir && !dirBlockers[filepath.Base(tool)] {
		sentinel := filepath.Join(workdir, ".lend-wait")
		if err := os.WriteFile(sentinel, nil, 0644); err == nil {
			cmdArgs = append(cmdArgs, sentinel)
		}
	}
	cmdArgs = append(cmdArgs, waitFlags[filepath.Base(tool)]...)

	cmd := exec.Command(tool, cmdArgs...)
	cmd.Dir = workdir
	cmd.Stdout = io.Discard
	cmd.Stderr = io.Discard
	if err := cmd.Start(); err != nil {
		w := bufio.NewWriter(conn)
		fmt.Fprintf(w, "RESULT 127\n")
		writeFrame(w, "SO", nil)
		writeFrame(w, "SE", []byte(err.Error()))
		_ = w.Flush()
		return
	}
	defer func() { _ = cmd.Process.Kill() }()

	bw := bufio.NewWriter(conn)
	if _, err := bw.WriteString("EDIT\n"); err != nil || bw.Flush() != nil {
		return
	}

	done := make(chan struct{})
	go func() {
		_ = cmd.Wait()
		close(done)
	}()

	lastF := make(map[int][]byte)
	lastD := make(map[int][]byte)
	for i := range args {
		switch args[i].kind {
		case "F":
			lastF[i] = args[i].data
		case "D":
			if b, err := writeTree(args[i].matPath); err == nil {
				lastD[i] = b
			}
		}
	}

	// syncArgs pushes any changed F/D args back to the client.
	syncArgs := func() {
		for i := range args {
			switch args[i].kind {
			case "F":
				data, err := os.ReadFile(args[i].matPath)
				if err != nil || bytes.Equal(data, lastF[i]) {
					continue
				}
				writeUpdate(bw, i, data)
				lastF[i] = data
			case "D":
				blob, err := writeTree(args[i].matPath)
				if err != nil || bytes.Equal(blob, lastD[i]) {
					continue
				}
				writeTreeFrame(bw, i, blob)
				lastD[i] = blob
			}
		}
	}

	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-done:
			// Editor closed: push any final unsynced change, then end.
			syncArgs()
			_, _ = bw.WriteString("SESSION_END\n")
			_ = bw.Flush()
			return
		case <-ticker.C:
			syncArgs()
			if bw.Flush() != nil {
				return // client disconnected
			}
		}
	}
}

// ---- wire helpers ----

func readLine(br *bufio.Reader) (string, error) {
	line, err := br.ReadString('\n')
	if err != nil {
		return "", err
	}
	return strings.TrimRight(line, "\r\n"), nil
}

func readN(br *bufio.Reader, n int) ([]byte, error) {
	if n < 0 || n > maxBlobSize {
		return nil, fmt.Errorf("invalid length %d", n)
	}
	buf := make([]byte, n)
	if _, err := io.ReadFull(br, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

func writeFrame(w *bufio.Writer, tag string, data []byte) {
	if data == nil {
		fmt.Fprintf(w, "%s -1\n", tag)
		return
	}
	fmt.Fprintf(w, "%s %d\n", tag, len(data))
	_, _ = w.Write(data)
}

func atoi(s string) int {
	n, _ := strconv.Atoi(s)
	return n
}

// ---- path safety ----

func safeName(name string) string {
	name = filepath.Base(name)
	name = filepath.Clean(name)
	if name == "" || name == "." || name == string(filepath.Separator) {
		return "arg"
	}
	return name
}

func safeRel(rel string) (string, error) {
	if rel == "" || filepath.IsAbs(rel) {
		return "", fmt.Errorf("invalid tree path %q", rel)
	}
	clean := filepath.Clean(rel)
	if clean == "." || clean == ".." || strings.HasPrefix(clean, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("invalid tree path %q", rel)
	}
	return clean, nil
}

// dedup returns a unique basename inside the workdir, appending ".N" before the
// extension when a previous arg already used the same name.
func dedup(used map[string]int, name string) string {
	n := used[name]
	if n == 0 {
		used[name] = 1
		return name
	}
	ext := filepath.Ext(name)
	stem := strings.TrimSuffix(name, ext)
	candidate := fmt.Sprintf("%s.%d%s", stem, n+1, ext)
	used[name] = n + 1
	used[candidate] = 1
	return candidate
}

// ---- directory tree codec ----
//
// Flat list of entries, each:
//   1 byte  type: 'F' file, 'D' directory, 'L' symlink, 'E' end
//   4 bytes big-endian length of the relative path, then the path bytes
//   'F' adds: 8 bytes big-endian content length, then content bytes
//   'L' adds: 4 bytes big-endian target length, then target bytes
// Terminated by a single 'E' byte.

func writeName(buf *bytes.Buffer, rel string) {
	var b [4]byte
	binary.BigEndian.PutUint32(b[:], uint32(len(rel)))
	buf.Write(b[:])
	buf.WriteString(rel)
}

func writeTree(root string) ([]byte, error) {
	var buf bytes.Buffer
	err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if path == root {
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		switch {
		case info.Mode()&os.ModeSymlink != 0:
			target, err := os.Readlink(path)
			if err != nil {
				return nil // skip unreadable symlink
			}
			buf.WriteByte('L')
			writeName(&buf, rel)
			var t [4]byte
			binary.BigEndian.PutUint32(t[:], uint32(len(target)))
			buf.Write(t[:])
			buf.WriteString(target)
		case info.IsDir():
			buf.WriteByte('D')
			writeName(&buf, rel)
		case info.Mode().IsRegular():
			data, err := os.ReadFile(path)
			if err != nil {
				return nil // skip unreadable file
			}
			buf.WriteByte('F')
			writeName(&buf, rel)
			var c [8]byte
			binary.BigEndian.PutUint64(c[:], uint64(len(data)))
			buf.Write(c[:])
			buf.Write(data)
		default:
			// skip socket/fifo/device
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	buf.WriteByte('E')
	return buf.Bytes(), nil
}

func readTree(blob []byte, dest string) error {
	r := bytes.NewReader(blob)
	for {
		typ, err := r.ReadByte()
		if err != nil {
			return fmt.Errorf("truncated tree: %w", err)
		}
		if typ == 'E' {
			return nil
		}
		nameLen, err := readU32(r)
		if err != nil {
			return err
		}
		name := make([]byte, nameLen)
		if _, err := io.ReadFull(r, name); err != nil {
			return err
		}
		rel, err := safeRel(string(name))
		if err != nil {
			return err
		}
		target := filepath.Join(dest, rel)
		switch typ {
		case 'D':
			if err := os.MkdirAll(target, 0755); err != nil {
				return err
			}
		case 'F':
			clen, err := readU64(r)
			if err != nil {
				return err
			}
			if clen > maxBlobSize {
				return fmt.Errorf("tree file too large: %d", clen)
			}
			content := make([]byte, clen)
			if _, err := io.ReadFull(r, content); err != nil {
				return err
			}
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			if err := os.WriteFile(target, content, 0644); err != nil {
				return err
			}
		case 'L':
			tlen, err := readU32(r)
			if err != nil {
				return err
			}
			tstr := make([]byte, tlen)
			if _, err := io.ReadFull(r, tstr); err != nil {
				return err
			}
			if err := os.MkdirAll(filepath.Dir(target), 0755); err != nil {
				return err
			}
			_ = os.Remove(target)
			if err := os.Symlink(string(tstr), target); err != nil {
				return err
			}
		default:
			return fmt.Errorf("unknown tree entry type %q", typ)
		}
	}
}

func readU32(r *bytes.Reader) (uint32, error) {
	var b [4]byte
	if _, err := io.ReadFull(r, b[:]); err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint32(b[:]), nil
}

func readU64(r *bytes.Reader) (uint64, error) {
	var b [8]byte
	if _, err := io.ReadFull(r, b[:]); err != nil {
		return 0, err
	}
	return binary.BigEndian.Uint64(b[:]), nil
}
