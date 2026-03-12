/**
 * Mock I2S Sink - Testing Interface
 * 
 * Replaces real I2S hardware output with an in-memory buffer for testing.
 * Captures audio data and collects statistics without requiring physical hardware.
 * 
 * Use this to validate:
 *  - SD card file loading
 *  - Audio mixing logic
 *  - Volume control
 *  - Channel routing (stereo/left/right)
 *  - Sample rate handling
 * 
 * Statistics collected:
 *  - Total samples written
 *  - Peak levels (left/right)
 *  - Clipping events
 *  - RMS levels
 *  - Zero-crossing rate
 */

#ifndef MOCK_I2S_SINK_H
#define MOCK_I2S_SINK_H

#include <Arduino.h>
#include "audio_config.h"

// Statistics structure
struct I2SStatistics {
    uint32_t totalSamplesWritten;    // Total stereo samples written
    int16_t peakLeft;                // Peak level left channel
    int16_t peakRight;               // Peak level right channel
    uint32_t clippingEventsLeft;     // Times left channel hit ±32767
    uint32_t clippingEventsRight;    // Times right channel hit ±32767
    float rmsLeft;                   // RMS level left channel (0.0-1.0)
    float rmsRight;                  // RMS level right channel (0.0-1.0)
    uint32_t zeroCrossingsLeft;      // Zero crossings (for frequency estimation)
    uint32_t zeroCrossingsRight;
    uint32_t silentSamples;          // Consecutive samples at zero
    uint32_t writeCallCount;         // Number of write() calls
    
    // Timing
    uint32_t firstWriteMs;           // millis() at first write
    uint32_t lastWriteMs;            // millis() at last write
    
    void reset() {
        totalSamplesWritten = 0;
        peakLeft = 0;
        peakRight = 0;
        clippingEventsLeft = 0;
        clippingEventsRight = 0;
        rmsLeft = 0.0f;
        rmsRight = 0.0f;
        zeroCrossingsLeft = 0;
        zeroCrossingsRight = 0;
        silentSamples = 0;
        writeCallCount = 0;
        firstWriteMs = 0;
        lastWriteMs = 0;
    }
};

/**
 * MockI2SSink - Drop-in replacement for I2S class
 * 
 * Compatible API with Arduino I2S library but writes to memory buffer instead.
 */
class MockI2SSink {
public:
    MockI2SSink();
    
    // I2S-compatible API
    void setBCLK(uint8_t pin) { _bclkPin = pin; }
    void setDATA(uint8_t pin) { _dataPin = pin; }
    void setBitsPerSample(uint8_t bits) { _bitsPerSample = bits; }
    
    bool begin(uint32_t sampleRate);
    void end();
    
    /**
     * Write stereo samples to mock buffer
     * Compatible with I2S.write(buffer, size)
     * 
     * @param buffer Pointer to int16_t samples [L, R, L, R, ...]
     * @param size Size in bytes (must be multiple of 4 for stereo 16-bit)
     * @return Number of bytes "written" (always equals size in mock mode)
     */
    size_t write(const int16_t* buffer, size_t size);
    
    /**
     * Write single sample (for compatibility with I2S.write(sample))
     * Collects samples in pairs (L, R) and updates statistics
     */
    size_t write(int16_t sample);
    
    // Statistics access
    const I2SStatistics& getStatistics() const { return _stats; }
    void resetStatistics() { _stats.reset(); }
    void printStatistics() const;
    
    // Buffer access (for verification)
    bool captureEnabled() const { return _captureEnabled; }
    void enableCapture(bool enable) { _captureEnabled = enable; }
    
    /**
     * Get captured samples (if capture enabled)
     * Returns interleaved L/R samples from internal buffer
     */
    const int16_t* getCapturedBuffer() const { return _captureBuffer; }
    size_t getCapturedSampleCount() const { return _captureCount; }
    size_t getCaptureBufferSize() const { return _captureBufferSize; }
    
    // Configuration
    bool isRunning() const { return _running; }
    uint32_t getSampleRate() const { return _sampleRate; }
    
private:
    // Configuration
    uint8_t _bclkPin;
    uint8_t _dataPin;
    uint8_t _bitsPerSample;
    uint32_t _sampleRate;
    bool _running;
    
    // Statistics
    I2SStatistics _stats;
    int16_t _lastSampleLeft;
    int16_t _lastSampleRight;
    uint64_t _rmsAccumLeft;
    uint64_t _rmsAccumRight;
    uint32_t _rmsCount;
    
    // Optional sample capture (for detailed inspection)
    bool _captureEnabled;
    int16_t* _captureBuffer;
    size_t _captureBufferSize;
    size_t _captureCount;
    
    // Single-sample write state
    bool _pendingSampleReady;
    int16_t _pendingSample;
    
    void updateStatistics(const int16_t* samples, size_t sampleCount);
};

#endif // MOCK_I2S_SINK_H
