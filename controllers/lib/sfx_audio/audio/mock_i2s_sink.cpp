/**
 * Mock I2S Sink - Implementation
 */

#if defined(SFX_HAS_AUDIO)

#include "mock_i2s_sink.h"
#include "audio_log.h"

// Default capture buffer size (stereo samples)
constexpr size_t DEFAULT_CAPTURE_SIZE = 4096;

MockI2SSink::MockI2SSink() 
    : _bclkPin(0)
    , _dataPin(0)
    , _bitsPerSample(16)
    , _sampleRate(44100)
    , _running(false)
    , _lastSampleLeft(0)
    , _lastSampleRight(0)
    , _rmsAccumLeft(0)
    , _rmsAccumRight(0)
    , _rmsCount(0)
    , _captureEnabled(false)
    , _captureBuffer(nullptr)
    , _captureBufferSize(DEFAULT_CAPTURE_SIZE)
    , _captureCount(0)
    , _pendingSampleReady(false)
    , _pendingSample(0)
{
    _stats.reset();
}

bool MockI2SSink::begin(uint32_t sampleRate) {
    _sampleRate = sampleRate;
    _running = true;
    _stats.reset();
    _lastSampleLeft = 0;
    _lastSampleRight = 0;
    _rmsAccumLeft = 0;
    _rmsAccumRight = 0;
    _rmsCount = 0;
    
    MOCK_LOG("Started (sampleRate=%lu Hz, bits=%d)", 
                  _sampleRate, _bitsPerSample);
    MOCK_LOG("MOCK MODE: Audio data will NOT be sent to hardware");
    MOCK_LOG("Statistics collection enabled");
    
    // Allocate capture buffer if needed
    if (_captureEnabled && !_captureBuffer) {
        _captureBuffer = new int16_t[_captureBufferSize * 2]; // Stereo
        _captureCount = 0;
        MOCK_LOG("Capture buffer allocated (%zu samples)", _captureBufferSize);
    }
    
    return true;
}

void MockI2SSink::end() {
    if (!_running) return;
    
    _running = false;
    
    // Free capture buffer
    if (_captureBuffer) {
        delete[] _captureBuffer;
        _captureBuffer = nullptr;
        _captureCount = 0;
    }
    
    MOCK_LOG("Stopped");
    printStatistics();
}

size_t MockI2SSink::write(const int16_t* buffer, size_t sizeBytes) {
    if (!_running || !buffer || sizeBytes == 0) {
        return 0;
    }
    
    // Size must be multiple of 4 (2 bytes per sample * 2 channels)
    if (sizeBytes % 4 != 0) {
        MOCK_LOG("WARNING: Write size %zu not multiple of 4", sizeBytes);
        return 0;
    }
    
    size_t sampleCount = sizeBytes / 4; // Stereo sample pairs
    
    // Update timing
    uint32_t now = millis();
    if (_stats.totalSamplesWritten == 0) {
        _stats.firstWriteMs = now;
    }
    _stats.lastWriteMs = now;
    
    _stats.writeCallCount++;
    _stats.totalSamplesWritten += sampleCount;
    
    // Update detailed statistics
    updateStatistics(buffer, sampleCount);
    
    // Capture samples if enabled
    if (_captureEnabled && _captureBuffer) {
        size_t samplesToCapture = min(sampleCount, _captureBufferSize - _captureCount);
        if (samplesToCapture > 0) {
            memcpy(_captureBuffer + (_captureCount * 2), buffer, samplesToCapture * 4);
            _captureCount += samplesToCapture;
        }
    }
    
    return sizeBytes; // Always "succeeds"
}

size_t MockI2SSink::write(int16_t sample) {
    if (!_running) {
        return 0;
    }
    
    if (!_pendingSampleReady) {
        // First sample of pair (left channel)
        _pendingSample = sample;
        _pendingSampleReady = true;
        return sizeof(int16_t);
    } else {
        // Second sample of pair (right channel) - process stereo pair
        _pendingSampleReady = false;
        
        int16_t pair[2] = { _pendingSample, sample };
        
        // Update timing
        uint32_t now = millis();
        if (_stats.totalSamplesWritten == 0) {
            _stats.firstWriteMs = now;
        }
        _stats.lastWriteMs = now;
        
        _stats.writeCallCount++;
        _stats.totalSamplesWritten++;
        
        // Update detailed statistics
        updateStatistics(pair, 1);
        
        // Capture samples if enabled
        if (_captureEnabled && _captureBuffer && _captureCount < _captureBufferSize) {
            _captureBuffer[_captureCount * 2] = pair[0];
            _captureBuffer[_captureCount * 2 + 1] = pair[1];
            _captureCount++;
        }
        
        return sizeof(int16_t);
    }
}

void MockI2SSink::updateStatistics(const int16_t* samples, size_t sampleCount) {
    for (size_t i = 0; i < sampleCount; i++) {
        int16_t left = samples[i * 2];
        int16_t right = samples[i * 2 + 1];
        
        // Peak detection
        if (abs(left) > abs(_stats.peakLeft)) {
            _stats.peakLeft = left;
        }
        if (abs(right) > abs(_stats.peakRight)) {
            _stats.peakRight = right;
        }
        
        // Clipping detection
        if (left == 32767 || left == -32768) {
            _stats.clippingEventsLeft++;
        }
        if (right == 32767 || right == -32768) {
            _stats.clippingEventsRight++;
        }
        
        // Zero crossing detection
        if ((_lastSampleLeft >= 0 && left < 0) || (_lastSampleLeft < 0 && left >= 0)) {
            _stats.zeroCrossingsLeft++;
        }
        if ((_lastSampleRight >= 0 && right < 0) || (_lastSampleRight < 0 && right >= 0)) {
            _stats.zeroCrossingsRight++;
        }
        
        // Silence detection
        if (left == 0 && right == 0) {
            _stats.silentSamples++;
        }
        
        // RMS accumulation (every 512 samples to avoid overflow)
        _rmsAccumLeft += (uint64_t)(left * left);
        _rmsAccumRight += (uint64_t)(right * right);
        _rmsCount++;
        
        if (_rmsCount >= 512) {
            // Calculate RMS and update running average
            float rmsL = sqrt(_rmsAccumLeft / (float)_rmsCount) / 32768.0f;
            float rmsR = sqrt(_rmsAccumRight / (float)_rmsCount) / 32768.0f;
            
            // Exponential moving average
            _stats.rmsLeft = _stats.rmsLeft * 0.95f + rmsL * 0.05f;
            _stats.rmsRight = _stats.rmsRight * 0.95f + rmsR * 0.05f;
            
            _rmsAccumLeft = 0;
            _rmsAccumRight = 0;
            _rmsCount = 0;
        }
        
        _lastSampleLeft = left;
        _lastSampleRight = right;
    }
}

void MockI2SSink::printStatistics() const {
    MOCK_LOG("========================================");
    MOCK_LOG("    MOCK I2S STATISTICS");
    MOCK_LOG("========================================");
    
    // Basic info
    MOCK_LOG("Sample Rate:       %lu Hz", _sampleRate);
    MOCK_LOG("Running:           %s", _running ? "YES" : "NO");
    
    // Write statistics
    MOCK_LOG("Write Calls:       %lu", _stats.writeCallCount);
    MOCK_LOG("Total Samples:     %lu stereo pairs", _stats.totalSamplesWritten);
    
    // Duration
    if (_stats.totalSamplesWritten > 0) {
        float durationSec = _stats.totalSamplesWritten / (float)_sampleRate;
        MOCK_LOG("Duration:          %.2f seconds", durationSec);
        
        if (_stats.lastWriteMs > _stats.firstWriteMs) {
            uint32_t elapsedMs = _stats.lastWriteMs - _stats.firstWriteMs;
            MOCK_LOG("Real Time:         %.2f seconds", elapsedMs / 1000.0f);
            MOCK_LOG("Realtime Ratio:    %.2fx", (durationSec * 1000.0f) / elapsedMs);
        }
    }
    
    // Levels
    MOCK_LOG("--- LEVELS ---");
    MOCK_LOG("Peak Left:         %d (%.1f dB)", 
              _stats.peakLeft, 
              20.0f * log10(abs(_stats.peakLeft) / 32768.0f));
    MOCK_LOG("Peak Right:        %d (%.1f dB)", 
              _stats.peakRight,
              20.0f * log10(abs(_stats.peakRight) / 32768.0f));
    MOCK_LOG("RMS Left:          %.3f (%.1f dB)", 
              _stats.rmsLeft,
              20.0f * log10(_stats.rmsLeft + 0.0001f));
    MOCK_LOG("RMS Right:         %.3f (%.1f dB)", 
              _stats.rmsRight,
              20.0f * log10(_stats.rmsRight + 0.0001f));
    
    // Quality indicators
    MOCK_LOG("--- QUALITY ---");
    MOCK_LOG("Clipping (L/R):    %lu / %lu events", 
              _stats.clippingEventsLeft, _stats.clippingEventsRight);
    
    if (_stats.totalSamplesWritten > 0) {
        float silencePercent = (_stats.silentSamples * 100.0f) / _stats.totalSamplesWritten;
        MOCK_LOG("Silent Samples:    %lu (%.1f%%)", 
                  _stats.silentSamples, silencePercent);
    }
    
    // Frequency estimation
    if (_stats.totalSamplesWritten > 0 && _stats.lastWriteMs > _stats.firstWriteMs) {
        float durationSec = _stats.totalSamplesWritten / (float)_sampleRate;
        float freqLeft = _stats.zeroCrossingsLeft / (2.0f * durationSec);
        float freqRight = _stats.zeroCrossingsRight / (2.0f * durationSec);
        MOCK_LOG("Est. Freq (L/R):   %.0f / %.0f Hz", freqLeft, freqRight);
    }
    
    // Capture info
    if (_captureEnabled) {
        MOCK_LOG("--- CAPTURE ---");
        MOCK_LOG("Captured Samples:  %zu / %zu", _captureCount, _captureBufferSize);
        MOCK_LOG("Buffer Full:       %s", 
                  _captureCount >= _captureBufferSize ? "YES" : "NO");
    }
    
    MOCK_LOG("========================================");
}

#endif // SFX_HAS_AUDIO
