// Virtual board — generic FILE_* / STREAM_* handlers.
//
// Any board that exposes a fauxfs.FS via Board.FS() gets full
// FILE_LIST / FILE_DOWNLOAD / FILE_MKDIR / FILE_DELETE / FILE_INFO /
// FILE_UPLOAD_BEGIN / FILE_UPLOAD_DATA / FILE_UPLOAD_END / FILE_UPLOAD_CANCEL
// for free. Boards that return nil from FS() are skipped — the
// dispatch falls through and they NACK if they don't handle the range
// in their own HandlePacket.
//
// Wire format references — mirrors the firmware's StorageServer:
//
//   FILE_LIST  / FILE_TREE / FILE_DOWNLOAD / FILE_INFO
//     payload: [pathLen:u8][path][target:u8]
//   FILE_DELETE / FILE_MKDIR
//     payload: [pathLen:u8][path][target:u8][flags:u8]
//   FILE_UPLOAD_BEGIN
//     payload: [size:u32LE][pathLen:u8][path][target:u8][mode:u8]
//   FILE_UPLOAD_DATA
//     payload: [seq:u16LE][crc16:u16LE][data]
//   FILE_UPLOAD_END / FILE_UPLOAD_CANCEL
//     payload: empty
//
// Streaming responses (FILE_LIST, FILE_DOWNLOAD) use the protocol
// stream packets:
//
//   STREAM_BEGIN: [totalBytes:u32LE]
//   STREAM_DATA : [seq:u16LE][crc16:u16LE][data]
//   STREAM_END  : [segs:u16LE][totalBytes:u32LE][crc16:u16LE]

package server

import (
	"fmt"

	"scalefx/protocol"
	pcore "scalefx/protocol/core"
	phub "scalefx/protocol/hubfx"
)

// handleFileIfFS returns true if the packet was a FILE_* / STREAM_*
// command and the board has an FS to service it.
func (s *Server) handleFileIfFS(ptype protocol.PacketType, tag byte, payload []byte) bool {
	if ptype < phub.FileList || ptype > phub.FileTree {
		return false
	}
	fs := s.board.FS()
	if fs == nil {
		// Board doesn't support storage — let the board's own
		// HandlePacket NACK it (or fall through to the global NACK).
		return false
	}

	switch ptype {
	case phub.FileList, phub.FileTree:
		s.handleFileList(tag, payload)
	case phub.FileDelete:
		s.handleFileDelete(tag, payload)
	case phub.FileMkdir:
		s.handleFileMkdir(tag, payload)
	case phub.FileInfo:
		s.handleFileInfo(tag, payload)
	case phub.FileDownload:
		s.handleFileDownload(tag, payload)
	case phub.FileUploadBegin:
		s.handleFileUploadBegin(tag, payload)
	case phub.FileUploadData:
		s.handleFileUploadData(tag, payload)
	case phub.FileUploadEnd:
		s.handleFileUploadEnd(tag, payload)
	case phub.FileUploadCancel:
		s.handleFileUploadCancel(tag, payload)
	default:
		return false
	}
	return true
}

// ─── List / Tree ───

// FILE_LIST returns its result as a stream of plain-text lines:
//
//   <type>\t<name>\t<size>\n
//
// where <type> is "f" for file or "d" for directory.
func (s *Server) handleFileList(tag byte, payload []byte) {
	path, target, ok := decodePathTarget(payload)
	if !ok {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	entries, err := s.board.FS().List(target, path)
	if err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	body := ""
	for _, e := range entries {
		t := "f"
		if e.IsDir {
			t = "d"
		}
		body += fmt.Sprintf("%s\t%s\t%d\n", t, e.Name, e.Size)
	}
	s.streamReply(tag, []byte(body))
}

// ─── Mkdir / Delete ───

func (s *Server) handleFileMkdir(tag byte, payload []byte) {
	path, target, flags, ok := decodePathTargetFlags(payload)
	if !ok {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	recursive := flags&0x01 != 0 // MkdirFlagParents
	if err := s.board.FS().Mkdir(target, path, recursive); err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	s.Ack(tag)
}

func (s *Server) handleFileDelete(tag byte, payload []byte) {
	path, target, flags, ok := decodePathTargetFlags(payload)
	if !ok {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	recursive := flags&0x01 != 0 // DeleteFlagRecursive
	if err := s.board.FS().Delete(target, path, recursive); err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	s.Ack(tag)
}

// ─── Info ───

func (s *Server) handleFileInfo(tag byte, payload []byte) {
	path, target, ok := decodePathTarget(payload)
	if !ok {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	st, err := s.board.FS().Stat(target, path)
	if err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	// FILE_INFO_RESP: [isDir:u8][size:u32LE][nameLen:u8][name]
	resp := make([]byte, 0, 6+len(st.Name))
	if st.IsDir {
		resp = append(resp, 1)
	} else {
		resp = append(resp, 0)
	}
	resp = append(resp, protocol.U32LE(st.Size)...)
	resp = append(resp, byte(len(st.Name)))
	resp = append(resp, []byte(st.Name)...)
	s.Send(phub.FileInfoResp, tag, resp)
}

// ─── Download ───

func (s *Server) handleFileDownload(tag byte, payload []byte) {
	path, target, ok := decodePathTarget(payload)
	if !ok {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	data, err := s.board.FS().Read(target, path)
	if err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	s.streamReply(tag, data)
}

// streamReply chunks `data` into STREAM_BEGIN/STREAM_DATA*/STREAM_END
// packets sharing the supplied tag. Used by both FILE_LIST and
// FILE_DOWNLOAD.
func (s *Server) streamReply(tag byte, data []byte) {
	const chunkSize = 256

	// STREAM_BEGIN: [totalBytes:u32LE]
	s.Send(protocol.StreamBegin, tag, protocol.U32LE(uint32(len(data))))

	var seq uint16
	for off := 0; off < len(data); off += chunkSize {
		end := off + chunkSize
		if end > len(data) {
			end = len(data)
		}
		chunk := data[off:end]
		// STREAM_DATA: [seq:u16LE][crc16:u16LE][data]
		body := make([]byte, 0, 4+len(chunk))
		body = append(body, protocol.U16LE(seq)...)
		body = append(body, protocol.U16LE(protocol.CRC16CCITT(chunk))...)
		body = append(body, chunk...)
		s.Send(protocol.StreamData, tag, body)
		seq++
	}

	// STREAM_END: [segs:u16LE][totalBytes:u32LE][crc16:u16LE]
	end := make([]byte, 0, 8)
	end = append(end, protocol.U16LE(seq)...)
	end = append(end, protocol.U32LE(uint32(len(data)))...)
	end = append(end, protocol.U16LE(protocol.CRC16CCITT(data))...)
	s.Send(protocol.StreamEnd, tag, end)
}

// ─── Upload state machine ───

func (s *Server) handleFileUploadBegin(tag byte, payload []byte) {
	if len(payload) < 5 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	size := protocol.ReadU32LE(payload, 0)
	pathLen := int(payload[4])
	if 5+pathLen+2 > len(payload) {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	path := string(payload[5 : 5+pathLen])
	target := payload[5+pathLen]
	mode := payload[6+pathLen]
	if err := s.board.FS().BeginUpload(target, path, size, mode); err != nil {
		s.Nack(tag, pcore.ErrBusy)
		return
	}
	s.Ack(tag)
}

func (s *Server) handleFileUploadData(tag byte, payload []byte) {
	if len(payload) < 4 {
		s.Nack(tag, pcore.ErrMissingParam)
		return
	}
	seq := protocol.ReadU16LE(payload, 0)
	// CRC at payload[2:4] — ignored; the firmware's accept-all faux FS
	// trusts the local TCP transport.
	if err := s.board.FS().AppendUploadChunk(seq, payload[4:]); err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	s.Ack(tag)
}

func (s *Server) handleFileUploadEnd(tag byte, _ []byte) {
	if err := s.board.FS().EndUpload(); err != nil {
		s.Nack(tag, pcore.ErrInvalidParam)
		return
	}
	s.Ack(tag)
}

func (s *Server) handleFileUploadCancel(tag byte, _ []byte) {
	s.board.FS().CancelUpload()
	s.Ack(tag)
}

// ─── Payload helpers ───

func decodePathTarget(p []byte) (string, byte, bool) {
	if len(p) < 1 {
		return "", 0, false
	}
	pathLen := int(p[0])
	if 1+pathLen+1 > len(p) {
		return "", 0, false
	}
	return string(p[1 : 1+pathLen]), p[1+pathLen], true
}

func decodePathTargetFlags(p []byte) (string, byte, byte, bool) {
	if len(p) < 1 {
		return "", 0, 0, false
	}
	pathLen := int(p[0])
	if 1+pathLen+2 > len(p) {
		return "", 0, 0, false
	}
	return string(p[1 : 1+pathLen]), p[1+pathLen], p[2+pathLen], true
}
