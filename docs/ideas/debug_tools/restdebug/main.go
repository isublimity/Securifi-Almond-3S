// restdebug — REST debug agent for Almond 3S
// Endpoints:
//   GET  /exec?cmd=...              — run shell command
//   GET  /mem?addr=0x...&n=4        — read n dwords
//   POST /mem?addr=0x...&val=0x...  — write dword
//   GET  /sm0                       — dump SM0 I2C registers
//   GET  /poll?sec=10               — poll SM0 for PIC transactions
//   POST /upload?path=/tmp/x        — upload file (body=raw)
//   GET  /download?path=/tmp/x      — download file
//   GET  /gpio?pin=515              — read GPIO
//   POST /gpio?pin=515&val=1        — write GPIO
//   GET  /dmesg?n=20&grep=bat       — tail dmesg
//
// Build: GOOS=linux GOARCH=mipsle go build -ldflags="-s -w" -o restdebug main.go
// Then: upx --best restdebug  (optional, shrinks ~3x)
package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const palmbus = 0x1E000000

var memfd int

func memRead(addr uint32) (uint32, error) {
	page := int64(addr & ^uint32(0xFFF))
	off := addr & 0xFFF
	b, err := syscall.Mmap(memfd, page, 0x1000, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return 0, err
	}
	val := *(*uint32)(unsafe.Pointer(&b[off]))
	syscall.Munmap(b)
	return val, nil
}

func memWrite(addr, val uint32) error {
	page := int64(addr & ^uint32(0xFFF))
	off := addr & 0xFFF
	b, err := syscall.Mmap(memfd, page, 0x1000, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_SHARED)
	if err != nil {
		return err
	}
	*(*uint32)(unsafe.Pointer(&b[off])) = val
	syscall.Munmap(b)
	return nil
}

func pb(off uint32) uint32 {
	v, _ := memRead(palmbus + off)
	return v
}

func reply(w http.ResponseWriter, data interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	json.NewEncoder(w).Encode(data)
}

func handleExec(w http.ResponseWriter, r *http.Request) {
	cmd := r.URL.Query().Get("cmd")
	if cmd == "" {
		reply(w, map[string]string{"error": "?cmd= required"})
		return
	}
	out, err := exec.Command("sh", "-c", cmd).CombinedOutput()
	errStr := ""
	if err != nil {
		errStr = err.Error()
	}
	reply(w, map[string]string{"cmd": cmd, "output": string(out), "error": errStr})
}

func handleMem(w http.ResponseWriter, r *http.Request) {
	addrStr := r.URL.Query().Get("addr")
	addr, err := strconv.ParseUint(addrStr, 0, 32)
	if err != nil {
		reply(w, map[string]string{"error": "bad addr"})
		return
	}

	if r.Method == "POST" {
		valStr := r.URL.Query().Get("val")
		val, err := strconv.ParseUint(valStr, 0, 32)
		if err != nil {
			reply(w, map[string]string{"error": "bad val"})
			return
		}
		memWrite(uint32(addr), uint32(val))
		rb, _ := memRead(uint32(addr))
		reply(w, map[string]interface{}{
			"addr": fmt.Sprintf("0x%08X", addr), "wrote": fmt.Sprintf("0x%08X", val),
			"readback": fmt.Sprintf("0x%08X", rb),
		})
		return
	}

	n, _ := strconv.Atoi(r.URL.Query().Get("n"))
	if n < 1 {
		n = 1
	}
	if n > 256 {
		n = 256
	}
	result := make([]map[string]string, n)
	for i := 0; i < n; i++ {
		a := uint32(addr) + uint32(i*4)
		v, _ := memRead(a)
		result[i] = map[string]string{
			"addr": fmt.Sprintf("0x%08X", a), "val": fmt.Sprintf("0x%08X", v),
		}
	}
	reply(w, result)
}

func handleSM0(w http.ResponseWriter, r *http.Request) {
	type reg struct {
		Name string `json:"name"`
		Off  string `json:"offset"`
		Val  string `json:"value"`
	}
	offsets := []struct {
		off  uint32
		name string
	}{
		{0x034, "RSTCTRL"}, {0x060, "GPIOMODE"},
		{0x900, "SM0_CFG"}, {0x904, "SM0_904"},
		{0x908, "SM0_DATA"}, {0x90C, "SM0_SLAVE"},
		{0x910, "SM0_DOUT"}, {0x914, "SM0_DIN"},
		{0x918, "SM0_POLL"}, {0x91C, "SM0_STAT"},
		{0x920, "SM0_STRT"}, {0x928, "SM0_CFG2"},
		{0x940, "N_CTL0"}, {0x944, "N_CTL1"},
		{0x948, "N_948"}, {0x94C, "N_94C"},
		{0x950, "N_D0"}, {0x954, "N_D1"},
	}
	regs := make([]reg, len(offsets))
	for i, o := range offsets {
		regs[i] = reg{o.name, fmt.Sprintf("0x%03X", o.off), fmt.Sprintf("0x%08X", pb(o.off))}
	}
	reply(w, regs)
}

func handlePoll(w http.ResponseWriter, r *http.Request) {
	sec, _ := strconv.Atoi(r.URL.Query().Get("sec"))
	if sec < 1 {
		sec = 5
	}
	if sec > 120 {
		sec = 120
	}

	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	flusher, _ := w.(http.Flusher)

	fmt.Fprintf(w, "Polling %ds...\n", sec)
	if flusher != nil {
		flusher.Flush()
	}

	var lastDin, lastData uint32
	total := sec * 100
	for i := 0; i < total; i++ {
		data := pb(0x908)
		din := pb(0x914)
		poll := pb(0x918)
		stat := pb(0x91C)
		strt := pb(0x920)
		cfg := pb(0x900)
		cfg2 := pb(0x928)
		ctl0 := pb(0x940)
		d0 := pb(0x950)
		d1 := pb(0x954)

		if data != lastData || din != lastDin || (data&0xFF) == 0x2A || (data&0xFF) == 0x48 {
			fmt.Fprintf(w, "[%d.%02d] DATA=%02X DIN=%08X POLL=%02X STAT=%02X STRT=%04X CFG=%08X CFG2=%X CTL0=%08X D0=%08X D1=%08X\n",
				i/100, i%100, data&0xFF, din, poll&0xFF, stat&0xFF, strt&0xFFFF, cfg, cfg2, ctl0, d0, d1)
			if flusher != nil {
				flusher.Flush()
			}
			lastData = data
			lastDin = din
		}
		time.Sleep(10 * time.Millisecond)
	}
	fmt.Fprintln(w, "Done.")
}

func handleUpload(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		reply(w, map[string]string{"error": "?path= required"})
		return
	}
	f, err := os.OpenFile(path, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0755)
	if err != nil {
		reply(w, map[string]string{"error": err.Error()})
		return
	}
	n, _ := io.Copy(f, r.Body)
	f.Close()
	reply(w, map[string]interface{}{"path": path, "bytes": n})
}

func handleDownload(w http.ResponseWriter, r *http.Request) {
	path := r.URL.Query().Get("path")
	if path == "" {
		http.Error(w, "?path= required", 400)
		return
	}
	w.Header().Set("Content-Disposition", "attachment; filename="+path[strings.LastIndex(path, "/")+1:])
	http.ServeFile(w, r, path)
}

func handleGPIO(w http.ResponseWriter, r *http.Request) {
	pin := r.URL.Query().Get("pin")
	if pin == "" {
		reply(w, map[string]string{"error": "?pin= required"})
		return
	}
	base := "/sys/class/gpio/gpio" + pin

	// Export if needed
	if _, err := os.Stat(base); os.IsNotExist(err) {
		os.WriteFile("/sys/class/gpio/export", []byte(pin), 0)
	}

	if r.Method == "POST" {
		val := r.URL.Query().Get("val")
		dir := r.URL.Query().Get("dir")
		if dir != "" {
			os.WriteFile(base+"/direction", []byte(dir), 0)
		}
		if val != "" {
			os.WriteFile(base+"/value", []byte(val), 0)
		}
	}

	dirB, _ := os.ReadFile(base + "/direction")
	valB, _ := os.ReadFile(base + "/value")
	reply(w, map[string]string{
		"pin": pin, "direction": strings.TrimSpace(string(dirB)),
		"value": strings.TrimSpace(string(valB)),
	})
}

func handleDmesg(w http.ResponseWriter, r *http.Request) {
	n := r.URL.Query().Get("n")
	if n == "" {
		n = "30"
	}
	grep := r.URL.Query().Get("grep")
	cmd := "dmesg | tail -" + n
	if grep != "" {
		cmd = "dmesg | grep -i '" + grep + "' | tail -" + n
	}
	out, _ := exec.Command("sh", "-c", cmd).CombinedOutput()
	w.Header().Set("Content-Type", "text/plain")
	w.Header().Set("Access-Control-Allow-Origin", "*")
	w.Write(out)
}

func main() {
	var err error
	memfd, err = syscall.Open("/dev/mem", syscall.O_RDWR|syscall.O_SYNC, 0)
	if err != nil {
		log.Fatalf("/dev/mem: %v", err)
	}

	http.HandleFunc("/exec", handleExec)
	http.HandleFunc("/mem", handleMem)
	http.HandleFunc("/sm0", handleSM0)
	http.HandleFunc("/poll", handlePoll)
	http.HandleFunc("/upload", handleUpload)
	http.HandleFunc("/download", handleDownload)
	http.HandleFunc("/gpio", handleGPIO)
	http.HandleFunc("/dmesg", handleDmesg)
	http.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/plain")
		fmt.Fprintln(w, "restdebug — Almond 3S Debug Agent")
		fmt.Fprintln(w, "GET  /exec?cmd=ls /tmp")
		fmt.Fprintln(w, "GET  /mem?addr=0x1E000900&n=24")
		fmt.Fprintln(w, "POST /mem?addr=0x1E000900&val=0xFA")
		fmt.Fprintln(w, "GET  /sm0")
		fmt.Fprintln(w, "GET  /poll?sec=10")
		fmt.Fprintln(w, "POST /upload?path=/tmp/test  (body=file)")
		fmt.Fprintln(w, "GET  /download?path=/tmp/test")
		fmt.Fprintln(w, "GET  /gpio?pin=515")
		fmt.Fprintln(w, "POST /gpio?pin=515&val=1&dir=out")
		fmt.Fprintln(w, "GET  /dmesg?n=20&grep=bat")
	})

	port := ":7777"
	log.Printf("restdebug listening on %s", port)
	log.Fatal(http.ListenAndServe(port, nil))
}
