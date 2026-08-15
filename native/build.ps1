# Bouwt alle native tools met MSVC (Visual Studio 2022 Community).
$vs = "C:\Program Files\Microsoft Visual Studio\2022\Community"
$vcvars = "$vs\VC\Auxiliary\Build\vcvars64.bat"
Push-Location $PSScriptRoot
cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /O2 /EHsc /std:c++17 /W3 passthrough.cpp /link ole32.lib avrt.lib && cl /nologo /O2 /EHsc /std:c++17 /W3 passthrough_ll.cpp /link ole32.lib avrt.lib winmm.lib && cl /nologo /O2 /EHsc /std:c++17 probe_iac3.cpp /link ole32.lib && cl /nologo /O2 /EHsc /std:c++17 probe_asio.cpp /link ole32.lib user32.lib && cl /nologo /O2 /EHsc /std:c++17 probe_padding.cpp /link ole32.lib winmm.lib"

# guitar_amp: vereist NAM (nam.lib). Eerst eenmalig NAM bouwen:
#   in third_party\NeuralAmpModelerCore\build_msvc:
#   cl /std:c++20 /O2 /arch:AVX2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN \
#      /DNAM_ENABLE_A2_FAST /I<root> /I<root>\Dependencies\eigen \
#      /I<root>\Dependencies\nlohmann /c <root>\NAM\*.cpp <root>\NAM\wavenet\*.cpp
#   lib /out:nam.lib *.obj
$nam = Resolve-Path "$PSScriptRoot\..\third_party\NeuralAmpModelerCore" -ErrorAction SilentlyContinue
$im = Resolve-Path "$PSScriptRoot\..\third_party\imgui" -ErrorAction SilentlyContinue
if ($nam -and (Test-Path "$nam\build_msvc\nam.lib")) {
    cmd /c "`"$vcvars`" >nul 2>&1 && cl /nologo /std:c++20 /O2 /arch:AVX2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNAM_ENABLE_A2_FAST /I`"$nam`" /I`"$nam\Dependencies\eigen`" /I`"$nam\Dependencies\nlohmann`" guitar_amp.cpp /link /WHOLEARCHIVE:`"$nam\build_msvc\nam.lib`" ole32.lib avrt.lib winmm.lib"
    if ($im) {
        cmd /c "`"$vcvars`" >nul 2>&1 && rc /nologo app.rc && cl /nologo /std:c++20 /O2 /arch:AVX2 /EHsc /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DNAM_ENABLE_A2_FAST /I`"$nam`" /I`"$nam\Dependencies\eigen`" /I`"$nam\Dependencies\nlohmann`" /I`"$im`" /I`"$im\backends`" guitar_amp_gui.cpp `"$im\imgui.cpp`" `"$im\imgui_draw.cpp`" `"$im\imgui_tables.cpp`" `"$im\imgui_widgets.cpp`" `"$im\backends\imgui_impl_win32.cpp`" `"$im\backends\imgui_impl_dx11.cpp`" /Fe:guitar_amp_gui.exe /link app.res /WHOLEARCHIVE:`"$nam\build_msvc\nam.lib`" ole32.lib avrt.lib winmm.lib d3d11.lib dxgi.lib user32.lib gdi32.lib shell32.lib /SUBSYSTEM:WINDOWS"
    }
} else {
    Write-Host "nam.lib niet gevonden - guitar_amp overgeslagen"
}
Pop-Location
