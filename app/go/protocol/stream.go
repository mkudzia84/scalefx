package protocol

// Streaming Protocol Constants (0xA4-0xA6)
// Transport-level chunked data streaming — mirrors serial/core/stream.h.
// Kept in protocol/ (not a sub-package) because connection.go uses these
// directly for stream reassembly.

const (
	StreamBegin PacketType = 0xA4
	StreamData  PacketType = 0xA5
	StreamEnd   PacketType = 0xA6
)

func init() {
	RegisterPacketNames(map[PacketType]string{
		StreamBegin: "STREAM_BEGIN",
		StreamData:  "STREAM_DATA",
		StreamEnd:   "STREAM_END",
	})
}
