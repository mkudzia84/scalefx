namespace ScaleFX.Serial.Console;

/// <summary>
/// Parses user input and routes it to the first matching
/// <see cref="ICommandHandler"/> in registration order.
/// </summary>
public class CommandDispatcher
{
    private readonly List<ICommandHandler> _handlers = new();

    public void Register(ICommandHandler handler) => _handlers.Add(handler);

    public async Task ExecuteAsync(string input, CommandContext context)
    {
        input = input.Trim();
        if (string.IsNullOrEmpty(input)) return;

        var parts = input.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        var command = parts[0].ToLowerInvariant();
        var args = parts.Length > 1 ? parts[1..] : Array.Empty<string>();

        if (command is "help" or "?")
        {
            if (args.Length > 0) ShowCommandHelp(args[0], context);
            else ShowHelp(context);
            return;
        }

        foreach (var handler in _handlers)
        {
            if (await handler.TryExecuteAsync(command, args, context))
                return;
        }

        context.Output.WriteError($"Unknown command: '{command}'. Type 'help' for available commands.");
    }

    public IEnumerable<(string Group, IReadOnlyList<CommandInfo> Commands)> GetGroupedCommands(string? detectedController = null)
    {
        foreach (var h in _handlers)
            if (h.IsRelevant(detectedController))
                yield return (h.GroupName, h.Commands);
    }

    private void ShowHelp(CommandContext context)
    {
        context.Output.WriteLine("Available commands:");
        context.Output.WriteLine();
        foreach (var h in _handlers)
        {
            if (!h.IsRelevant(context.DetectedController)) continue;
            context.Output.WriteHeading($"── {h.GroupName} ──", h.GroupName);
            foreach (var c in h.Commands)
            {
                int pad = Math.Max(1, 32 - c.Usage.Length);
                context.Output.WriteLine($"  {c.Usage}{new string(' ', pad)}{c.Description}");
            }
            context.Output.WriteLine();
        }
    }

    private void ShowCommandHelp(string name, CommandContext context)
    {
        foreach (var h in _handlers)
        {
            if (!h.IsRelevant(context.DetectedController)) continue;
            foreach (var c in h.Commands)
                if (c.Name.Equals(name, StringComparison.OrdinalIgnoreCase))
                {
                    context.Output.WriteInfo(c.Usage);
                    context.Output.WriteLine($"  {c.Description}");
                    return;
                }
        }
        context.Output.WriteError($"Unknown command: '{name}'");
    }
}
