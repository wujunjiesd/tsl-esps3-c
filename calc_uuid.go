package main

import (
	"encoding/hex"
	"fmt"
	"strings"
)

func reverse(b []byte) []byte {
	l := len(b)
	r := make([]byte, l)
	for i := 0; i < l; i++ {
		r[l-i-1] = b[i]
	}
	return r
}

func parseUUID(s string) []byte {
	s = strings.Replace(s, "-", "", -1)
	b, _ := hex.DecodeString(s)
	return reverse(b)
}

func formatC(b []byte) string {
	parts := make([]string, len(b))
	for i, v := range b {
		parts[i] = fmt.Sprintf("0x%02x", v)
	}
	return strings.Join(parts, ", ")
}

func main() {
	// From Go SDK ble.go lines 33-35
	svcUUID := "00000211-b2d1-43f0-9b88-960cebf8b91e"
	writeUUID := "00000212-b2d1-43f0-9b88-960cebf8b91e"
	readUUID := "00000213-b2d1-43f0-9b88-960cebf8b91e"

	fmt.Println("// Tesla Service UUID:", svcUUID)
	svc := parseUUID(svcUUID)
	fmt.Println("static const ble_uuid128_t tesla_svc_uuid = BLE_UUID128_INIT(")
	fmt.Println("   ", formatC(svc))
	fmt.Println(");")

	fmt.Println()
	fmt.Println("// Write Characteristic UUID (toVehicle):", writeUUID)
	write := parseUUID(writeUUID)
	fmt.Println("static const ble_uuid128_t tesla_write_chr_uuid = BLE_UUID128_INIT(")
	fmt.Println("   ", formatC(write))
	fmt.Println(");")

	fmt.Println()
	fmt.Println("// Read Characteristic UUID (fromVehicle):", readUUID)
	read := parseUUID(readUUID)
	fmt.Println("static const ble_uuid128_t tesla_read_chr_uuid = BLE_UUID128_INIT(")
	fmt.Println("   ", formatC(read))
	fmt.Println(");")
}
