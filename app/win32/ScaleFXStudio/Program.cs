namespace ScaleFXStudio;

static class Program
{
    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool AllocConsole();

    [STAThread]
    static void Main(string[] args)
    {
        // Attach console window for debug output
        AllocConsole();
        Console.WriteLine("ScaleFX Studio - Debug Console");
        Console.WriteLine("===============================");
        Console.WriteLine();
        
        try
        {
            ApplicationConfiguration.Initialize();
            
            var mainForm = new MainForm();
            
            // If a file path was passed as argument, open it
            if (args.Length > 0 && File.Exists(args[0]))
            {
                try
                {
                    var configService = new Services.ConfigService();
                    var config = configService.Load(args[0]);
                    // Would need to pass this to MainForm - for now just start empty
                }
                catch
                {
                    // Ignore load errors on startup
                }
            }
            
            Application.Run(mainForm);
        }
        catch (Exception ex)
        {
            MessageBox.Show($"Fatal Error: {ex.Message}\n\n{ex.StackTrace}", "ScaleFXStudio Error", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }
}
