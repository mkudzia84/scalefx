namespace ScaleFX.Serial.Console;

/// <summary>
/// Abstraction for console output. Command handlers write formatted text
/// through this interface without depending on any UI framework.
/// </summary>
public interface IConsoleOutput
{
    void Write(string text);
    void WriteLine(string text = "");
    void WriteSuccess(string text);
    void WriteError(string text);
    void WriteWarning(string text);
    void WriteInfo(string text);
    void WriteData(string label, string value);

    /// <summary>
    /// Writes a section heading, optionally associated with a command group
    /// (e.g., "Core", "GunFX") for group-specific colorization.
    /// </summary>
    void WriteHeading(string text, string? group = null) => WriteInfo(text);
}
