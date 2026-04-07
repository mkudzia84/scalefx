package main

import (
	"embed"

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

func main() {
	app := NewApp()

	err := wails.Run(&options.App{
		Title:     "ScaleFX Studio",
		Width:     1200,
		Height:    800,
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
		Bind: []interface{}{
			app,
		},
	})

	if err != nil {
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

	viewMenu := m.AddSubmenu("View")
	viewMenu.AddText("Console", keys.CmdOrCtrl("`"), func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:console")
	})

	helpMenu := m.AddSubmenu("Help")
	helpMenu.AddText("About ScaleFX Studio", nil, func(_ *menu.CallbackData) {
		wailsRT.EventsEmit(app.ctx, "menu:about")
	})

	return m
}
