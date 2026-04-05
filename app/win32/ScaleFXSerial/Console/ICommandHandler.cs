namespace ScaleFX.Serial.Console;

/// <summary>
/// A group of related CLI commands (e.g., Core, GunFX, LightFX).
/// Handlers are registered with <see cref="CommandDispatcher"/> and
/// tried in order until one handles the input.
/// </summary>
public interface ICommandHandler
{
    /// <summary>Display name for the command group (shown in help).</summary>
    string GroupName { get; }

    /// <summary>All commands this handler can process.</summary>
    IReadOnlyList<CommandInfo> Commands { get; }

    /// <summary>
    /// Attempts to execute <paramref name="command"/>. Returns true if
    /// the command was recognized (even if it failed), false to let the
    /// next handler try.
    /// </summary>
    Task<bool> TryExecuteAsync(string command, string[] args, CommandContext context);

    /// <summary>
    /// Returns true if this handler's commands are relevant for the given
    /// controller type. Used by the dispatcher to filter help output.
    /// When <paramref name="detectedController"/> is null (not yet connected),
    /// all handlers are shown.
    /// </summary>
    bool IsRelevant(string? detectedController) => true;
}
