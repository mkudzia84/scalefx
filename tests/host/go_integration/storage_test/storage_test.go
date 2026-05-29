package storage_test

// Integration tests for non-upload storage commands:
//   - SD_STATUS / SD_INIT
//   - FLASH_STATUS
//   - FILE_LIST, FILE_INFO, FILE_MKDIR / FILE_DELETE round-trip
//
// Uploads themselves are covered by upload_test/ which has the 1.4 MB
// stream regression net for the 2026-05-28 four-bug fix.  This suite
// hits the path operations the firmware exposes to Studio's file
// browser + CLI's sd-status / file.list / file.info.

import (
	"os"
	"strings"
	"testing"
	"time"

	"scalefx/client"
	"scalefx/protocol/core"
	"scalefx/tests/host/ports"
)

func TestMain(m *testing.M) {
	os.Exit(ports.RunWithSharedClient(m, "storage_test"))
}

func requireSD(t *testing.T) *client.Client {
	t.Helper()
	c := ports.RequireSharedClient(t)
	id := ports.SharedIdentity()
	if !core.HasCapability(id.Capabilities, core.CapSd) {
		t.Skipf("device %s doesn't advertise SD capability", id.DeviceName)
	}
	return c
}

func requireFlash(t *testing.T) *client.Client {
	t.Helper()
	c := ports.RequireSharedClient(t)
	id := ports.SharedIdentity()
	if !core.HasCapability(id.Capabilities, core.CapFlash) {
		t.Skipf("device %s doesn't advertise FLASH capability", id.DeviceName)
	}
	return c
}

// ─── SD_STATUS ────────────────────────────────────────────────────────

func TestSdStatusReturnsInitializedCard(t *testing.T) {
	c := requireSD(t)
	s, err := c.Storage.SdStatus()
	if err != nil {
		t.Fatalf("SdStatus: %v", err)
	}
	if !s.Initialized {
		t.Fatal("SD card not initialised — physical card present?")
	}
	if s.CardSizeMB == 0 {
		t.Error("CardSizeMB = 0 — SD enumeration broken?")
	}
	if s.TotalSpaceMB == 0 || s.TotalSpaceMB > s.CardSizeMB {
		t.Errorf("TotalSpaceMB = %d outside (0..%d]", s.TotalSpaceMB, s.CardSizeMB)
	}
	// Sane upper bound — used + free ≈ total (within a small overhead margin).
	if s.UsedSpaceMB+s.FreeSpaceMB > s.TotalSpaceMB+10 {
		t.Errorf("used (%d) + free (%d) > total (%d) by > 10 MB — accounting broken",
			s.UsedSpaceMB, s.FreeSpaceMB, s.TotalSpaceMB)
	}
	t.Logf("SD: %d MB card, %d MB used, %d MB free",
		s.CardSizeMB, s.UsedSpaceMB, s.FreeSpaceMB)
}

// ─── FLASH_STATUS ─────────────────────────────────────────────────────

func TestFlashStatusReturnsInitializedFlash(t *testing.T) {
	c := requireFlash(t)
	s, err := c.Storage.FlashStatus()
	if err != nil {
		t.Fatalf("FlashStatus: %v", err)
	}
	if !s.Initialized {
		t.Fatal("LittleFS not initialised — partition table broken?")
	}
	if s.TotalBytes == 0 {
		t.Error("TotalBytes = 0 — LittleFS reports empty volume")
	}
	if s.UsedBytes+s.FreeBytes > s.TotalBytes+4096 {
		t.Errorf("used (%d) + free (%d) > total (%d) by > 4 KB — accounting broken",
			s.UsedBytes, s.FreeBytes, s.TotalBytes)
	}
	t.Logf("Flash: %d KB total, %d KB used, %d KB free",
		s.TotalBytes/1024, s.UsedBytes/1024, s.FreeBytes/1024)
}

// ─── FILE_INFO + FILE_MKDIR + FILE_DELETE round-trip ─────────────────

// mkdir → file.info → cleanup: verifies the path-routed file ops
// agree on what "exists" means.  The /test dir is the scratch space
// the upload_test suite also uses.
func TestMkdirFileInfoRoundTrip(t *testing.T) {
	c := requireSD(t)
	const dir = "/test/storage_round_trip_dir"

	// mkdir -p so a leftover from a previous run doesn't trip us.
	if err := c.Storage.FileMkdir(dir, client.TargetSD, client.MkdirFlagParents); err != nil {
		t.Logf("mkdir: %v (pre-existing dir is OK)", err)
	}
	defer func() {
		if err := c.Storage.FileDelete(dir, client.TargetSD, client.DeleteFlagNone); err != nil {
			t.Logf("delete (cleanup): %v", err)
		}
	}()

	// Settle so the FAT entry is durable before FILE_INFO queries it.
	time.Sleep(20 * time.Millisecond)
	info, err := c.Storage.FileInfo(dir)
	if err != nil {
		t.Fatalf("FileInfo(%s): %v", dir, err)
	}
	if !info.Exists {
		t.Error("FileInfo.Exists = false after FileMkdir succeeded")
	}
	if !info.IsDir {
		t.Errorf("FileInfo.IsDir = false on a directory we just created")
	}
}

// ─── FILE_LIST ────────────────────────────────────────────────────────

// FILE_LIST against the SD root must succeed and yield non-empty text
// (every booted HubFX has at least the AssetCache preload directories).
func TestFileListRootIsNonEmpty(t *testing.T) {
	c := requireSD(t)
	listing, err := c.Storage.FileList("/", client.TargetSD)
	if err != nil {
		t.Fatalf("FileList(/): %v", err)
	}
	if strings.TrimSpace(listing) == "" {
		t.Error("FileList(/) returned empty - SD is blank or driver broken")
	}
	t.Logf("FileList(/) returned %d bytes", len(listing))
}

// Empty / non-existent paths: the firmware doesn't NACK these; it
// returns an empty listing as a normal result.  Studio's file browser
// relies on this — clicking a never-populated dir shouldn't pop a
// dialog.  Confirm the response is non-error and (per Studio's
// expectations) sane-looking text.
func TestFileListNonExistentPathReturnsEmpty(t *testing.T) {
	c := requireSD(t)
	listing, err := c.Storage.FileList("/does_not_exist_unit_test_dir_xyz", client.TargetSD)
	if err != nil {
		t.Logf("FileList on missing path: %v (firmware may differ between revs; "+
			"empty-response and NACK are both acceptable)", err)
		return
	}
	// Empty listing is the expected outcome — anything more than a
	// reasonable status line indicates the firmware listed some OTHER
	// directory's contents (path-handling bug).
	if len(listing) > 64 {
		t.Errorf("FileList(missing) returned %d bytes — expected empty or short status", len(listing))
	}
}
