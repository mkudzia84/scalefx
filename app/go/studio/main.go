package main

import (
	"embed"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/wailsapp/wails/v2"
	"github.com/wailsapp/wails/v2/pkg/menu"
	"github.com/wailsapp/wails/v2/pkg/menu/keys"
	"github.com/wailsapp/wails/v2/pkg/options"
	"github.com/wailsapp/wails/v2/pkg/options/assetserver"
	"github.com/wailsapp/wails/v2/pkg/options/windows"
	wailsRT "github.com/wailsapp/wails/v2/pkg/runtime"
)

//go:embed all:frontend/dist
var assets embed.FS

// startupTrace writes a one-line marker into the same log file the diag
// system uses, BEFORE Wails takes over. Lets us tell whether wails.Run
// even reached the OnStartup callback when troubleshooting GUI launch
// failures (Studio is `-H windowsgui` so stdout / stderr go nowhere).
func startupTrace(stage string) {
	path := filepath.Join(os.TempDir(), "scalefx-studio.log")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0o644)
	if err != nil {
		return
	}
	defer f.Close()
	fmt.Fprintf(f, "[%s] TRACE main:%s pid=%d\n",
		time.Now().Format("15:04:05.000"), stage, os.Getpid())
}

func main() {
	startupTrace("enter")
	app := NewApp()
	startupTrace("after-NewApp")

	startupTrace("before-wails.Run")
	err := wails.Run(&options.App{
		Title:     "ScaleFX Studio",
		Width:     1400,
		Height:    900,
		MinWidth:  800,
		MinHeight: 500,
		AssetServer: &assetserver.Options{
			Assets: assets,
		},
		BackgroundColour: &options.RGBA{R: 30, G: 30, B: 30, A: 1},
		OnStartup:        app.startup,
		OnShutdown:       app.shutdown,
		Menu:             createMenu(app),
		Windows: &windows.Options{
			WebviewIsTransparent: false,
			WindowIsTranslucent:  false,
			Theme:                windows.SystemDefault,
		},
		DragAndDrop: &options.DragAndDrop{
			EnableFileDrop:     true,
			DisableWebViewDrop: true,
			CSSDropProperty:    "--wails-drop-target",
			CSSDropValue:       "drop",
		},
		Bind: []interface{}{
			app,
		},
	})

	startupTrace("after-wails.Run")
	if err != nil {
		startupTrace("ERROR " + err.Error())
		println("Error:", err.Error())
	}
}

func createMenu(app *App) *menu.Menu {
	m := menu.NewMenu()

	fileMenu := m.AddSubmenu("File")
	fileMenu.AddText("Connect...", keys.CmdOrCtrl("k"), func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:connect")
	})
	fileMenu.AddText("Disconnect", nil, func(_ *menu.CallbackData) {
		go app.Disconnect()
	})
	fileMenu.AddSeparator()
	fileMenu.AddText("Exit", keys.CmdOrCtrl("q"), func(_ *menu.CallbackData) {
		wailsRT.Quit(app.ctx)
	})

	toolsMenu := m.AddSubmenu("Tools")
	toolsMenu.AddText("File Manager...", keys.CmdOrCtrl("m"), func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:filemanager")
	})

	viewMenu := m.AddSubmenu("View")
	viewMenu.AddText("Console", keys.CmdOrCtrl("`"), func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:console")
	})
	viewMenu.AddSeparator()
	viewMenu.AddText("Settings...", keys.CmdOrCtrl(","), func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:viewsettings")
	})

	helpMenu := m.AddSubmenu("Help")
	helpMenu.AddText("About ScaleFX Studio", nil, func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:about")
	})

	return m
}
