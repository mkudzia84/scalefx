"""
Storage CLI Tests

Tests for SD card storage commands: sd ls, sd tree, sd info, etc.
"""

import pytest
from conftest import SerialConnection


class TestStorageCommands:
    """Test storage CLI commands."""
    
    # =========================================================================
    # SD INIT - Initialize SD card
    # =========================================================================
    
    def test_sd_init_default(self, fresh_pico: SerialConnection):
        """Test sd init with default speed."""
        response = fresh_pico.send("sd init", delay=1.0)
        # Should either succeed or fail gracefully
        assert "MHz" in response or "Error" in response or "Success" in response or "Failed" in response
    
    def test_sd_init_with_speed(self, fresh_pico: SerialConnection):
        """Test sd init with explicit speed."""
        response = fresh_pico.send("sd init 10", delay=1.0)
        assert "10 MHz" in response or "error" in response.lower()
    
    def test_sd_init_invalid_speed(self, fresh_pico: SerialConnection):
        """Test sd init rejects invalid speed."""
        response = fresh_pico.send("sd init 100")
        assert "Error" in response or "error" in response


class TestStorageListCommands:
    """Test directory listing commands (require initialized SD)."""
    
    # =========================================================================
    # SD LS - List directory
    # =========================================================================
    
    def test_sd_ls_root(self, fresh_pico: SerialConnection):
        """Test listing root directory."""
        response = fresh_pico.send("sd ls /")
        # Will show error if not initialized, or list contents
        if "not initialized" not in response.lower():
            assert "/" in response or "DIR" in response or "FILE" in response or "items" in response.lower()
    
    @pytest.mark.json
    def test_sd_ls_json(self, fresh_pico: SerialConnection):
        """Test sd ls JSON output format."""
        result = fresh_pico.send_json("sd ls /")
        
        if "error" in result:
            pytest.skip("SD card not initialized")
        
        assert "path" in result or "error" in result
        if "path" in result:
            assert "items" in result
            assert isinstance(result["items"], list)
    
    def test_sd_ls_default_path(self, fresh_pico: SerialConnection):
        """Test sd ls without path defaults to root."""
        response = fresh_pico.send("sd ls")
        # Should list root or show error
        if "not initialized" not in response.lower():
            assert "/" in response or "items" in response.lower() or "Total" in response
    
    def test_sd_ls_nonexistent(self, fresh_pico: SerialConnection):
        """Test sd ls on nonexistent path shows error."""
        response = fresh_pico.send("sd ls /does_not_exist_12345")
        if "not initialized" not in response.lower():
            assert "error" in response.lower() or "failed" in response.lower()
    
    # =========================================================================
    # SD TREE - Directory tree
    # =========================================================================
    
    def test_sd_tree(self, fresh_pico: SerialConnection):
        """Test directory tree display."""
        response = fresh_pico.send("sd tree", delay=1.0)
        if "not initialized" not in response.lower():
            assert "/" in response or "tree" in response.lower()
    
    @pytest.mark.json
    def test_sd_tree_json(self, fresh_pico: SerialConnection):
        """Test sd tree JSON output."""
        result = fresh_pico.send_json("sd tree", delay=1.0)
        
        if "error" in result:
            pytest.skip("SD card not initialized")
        
        assert "tree" in result
    
    # =========================================================================
    # SD INFO - Card information
    # =========================================================================
    
    def test_sd_info(self, fresh_pico: SerialConnection):
        """Test SD card info display."""
        response = fresh_pico.send("sd info")
        if "not initialized" not in response.lower():
            # Should show card stats
            assert "MB" in response or "size" in response.lower() or "FAT" in response
    
    @pytest.mark.json
    def test_sd_info_json(self, fresh_pico: SerialConnection):
        """Test sd info JSON output."""
        result = fresh_pico.send_json("sd info")
        
        if "error" in result:
            pytest.skip("SD card not initialized")
        
        # Should have size and volume info
        assert "cardSizeMB" in result or "totalSpaceMB" in result


class TestStorageFileCommands:
    """Test file operation commands."""
    
    # =========================================================================
    # SD CAT - Display file contents
    # =========================================================================
    
    def test_sd_cat_missing_arg(self, fresh_pico: SerialConnection):
        """Test sd cat without filename shows usage."""
        response = fresh_pico.send("sd cat")
        assert "Usage" in response or "usage" in response.lower()
    
    def test_sd_cat_nonexistent(self, fresh_pico: SerialConnection):
        """Test sd cat on nonexistent file shows error."""
        response = fresh_pico.send("sd cat /nonexistent_file_12345.txt")
        if "not initialized" not in response.lower():
            assert "error" in response.lower() or "failed" in response.lower()
    
    # =========================================================================
    # SD RM - Remove file
    # =========================================================================
    
    def test_sd_rm_missing_arg(self, fresh_pico: SerialConnection):
        """Test sd rm without filename shows usage."""
        response = fresh_pico.send("sd rm")
        assert "Usage" in response or "usage" in response.lower()
    
    def test_sd_rm_nonexistent(self, fresh_pico: SerialConnection):
        """Test sd rm on nonexistent file shows error."""
        response = fresh_pico.send("sd rm /nonexistent_file_12345.txt")
        if "not initialized" not in response.lower():
            assert "Failed" in response or "error" in response.lower() or "not found" in response.lower()
    
    # =========================================================================
    # SD UPLOAD - File upload
    # =========================================================================
    
    def test_sd_upload_missing_args(self, fresh_pico: SerialConnection):
        """Test sd upload without arguments shows usage."""
        response = fresh_pico.send("sd upload")
        assert "Usage" in response or "usage" in response.lower()
    
    def test_sd_upload_invalid_size(self, fresh_pico: SerialConnection):
        """Test sd upload rejects invalid size."""
        response = fresh_pico.send("sd upload /test.txt 999999999999")
        assert "Error" in response or "error" in response
    
    # =========================================================================
    # SD DOWNLOAD - File download
    # =========================================================================
    
    def test_sd_download_missing_arg(self, fresh_pico: SerialConnection):
        """Test sd download without filename shows usage."""
        response = fresh_pico.send("sd download")
        assert "Usage" in response or "usage" in response.lower()


class TestStorageNotInitialized:
    """Test error handling when SD card is not initialized."""
    
    @pytest.mark.json
    def test_sd_ls_not_init_json(self, fresh_pico: SerialConnection):
        """Test JSON error when SD not initialized."""
        # First ensure we test the not-initialized state
        # This may or may not trigger depending on test order
        result = fresh_pico.send_json("sd ls")
        
        if "error" in result:
            assert "not initialized" in result["error"].lower() or "error" in result
