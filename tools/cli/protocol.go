package main

// ScaleFX CLI - Protocol Layer
// COBS encode/decode, CRC-8/CRC-16, packet build/parse, endian helpers.

// CRC8 calculates CRC-8 checksum (polynomial 0x07).
func CRC8(data []byte) byte {
	var crc byte
	for _, b := range data {
		crc ^= b
		for i := 0; i < 8; i++ {
			if crc&0x80 != 0 {
				crc = (crc << 1) ^ 0x07
			} else {
				crc = crc << 1
			}
		}
	}
	return crc
}

// CRC16CCITT calculates CRC-16/CCITT (polynomial 0x1021, init 0xFFFF).
func CRC16CCITT(data []byte) uint16 {
	crc := uint16(0xFFFF)
	for _, b := range data {
		crc ^= uint16(b) << 8
		for i := 0; i < 8; i++ {
			if crc&0x8000 != 0 {
				crc = (crc << 1) ^ 0x1021
			} else {
				crc = crc << 1
			}
		}
	}
	return crc
}

// COBSEncode encodes data using Consistent Overhead Byte Stuffing.
// Returns encoded bytes without the 0x00 delimiter.
func COBSEncode(data []byte) []byte {
	if len(data) == 0 {
		return []byte{0x01}
	}

	output := make([]byte, 0, len(data)+len(data)/254+2)
	output = append(output, 0) // placeholder for first code
	codeIdx := 0
	code := byte(1)

	for _, b := range data {
		if b == 0 {
			output[codeIdx] = code
			codeIdx = len(output)
			output = append(output, 0) // placeholder for next code
			code = 1
		} else {
			output = append(output, b)
			code++
			if code == 0xFF {
				output[codeIdx] = code
				codeIdx = len(output)
				output = append(output, 0)
				code = 1
			}
		}
	}

	output[codeIdx] = code
	return output
}

// COBSDecode decodes COBS-encoded data. Returns nil if invalid.
func COBSDecode(data []byte) []byte {
	if len(data) == 0 {
		return nil
	}

	output := make([]byte, 0, len(data))
	idx := 0

	for idx < len(data) {
		code := data[idx]
		if code == 0 {
			return nil // invalid: 0x00 in encoded data
		}
		idx++

		for i := byte(1); i < code; i++ {
			if idx >= len(data) {
				return nil // truncated
			}
			output = append(output, data[idx])
			idx++
		}

		if code < 0xFF && idx < len(data) {
			output = append(output, 0)
		}
	}

	return output
}

// BuildPacket builds a complete COBS-encoded packet with 0x00 delimiter.
// Packet structure (pre-COBS): [type:u8][tag:u8][len:u16LE][payload:0-512][crc8:u8]
func BuildPacket(packetType byte, payload []byte, tag byte) []byte {
	length := len(payload)
	raw := make([]byte, 0, 4+length+1)
	raw = append(raw, packetType, tag, byte(length&0xFF), byte((length>>8)&0xFF))
	raw = append(raw, payload...)
	raw = append(raw, CRC8(raw))

	encoded := COBSEncode(raw)
	return append(encoded, 0x00)
}

// ParsePacket decodes a COBS packet and validates CRC.
// Returns (packetType, tag, payload, ok).
func ParsePacket(data []byte) (byte, byte, []byte, bool) {
	// Remove delimiter if present
	if len(data) > 0 && data[len(data)-1] == 0x00 {
		data = data[:len(data)-1]
	}

	decoded := COBSDecode(data)
	if decoded == nil || len(decoded) < 5 {
		return 0, 0, nil, false
	}

	packetType := decoded[0]
	tag := decoded[1]
	length := int(decoded[2]) | int(decoded[3])<<8

	if len(decoded) != 5+length {
		return 0, 0, nil, false // length mismatch
	}

	payload := decoded[4 : 4+length]
	receivedCRC := decoded[len(decoded)-1]
	expectedCRC := CRC8(decoded[:len(decoded)-1])

	if receivedCRC != expectedCRC {
		return 0, 0, nil, false // CRC mismatch
	}

	return packetType, tag, payload, true
}

// --- Endian helpers ---

func U16LE(v uint16) []byte {
	return []byte{byte(v & 0xFF), byte((v >> 8) & 0xFF)}
}

func U32LE(v uint32) []byte {
	return []byte{
		byte(v & 0xFF),
		byte((v >> 8) & 0xFF),
		byte((v >> 16) & 0xFF),
		byte((v >> 24) & 0xFF),
	}
}

func ReadU16LE(data []byte, offset int) uint16 {
	return uint16(data[offset]) | uint16(data[offset+1])<<8
}

func ReadI16LE(data []byte, offset int) int16 {
	return int16(ReadU16LE(data, offset))
}

func ReadU32LE(data []byte, offset int) uint32 {
	return uint32(data[offset]) |
		uint32(data[offset+1])<<8 |
		uint32(data[offset+2])<<16 |
		uint32(data[offset+3])<<24
}
