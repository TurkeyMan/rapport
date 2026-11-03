# Fetches the NuGet packages the WinUI spike builds against into a folder of
# their own (never the source tree): the Windows App SDK (which brings WinUI,
# Foundation and the rest) and C++/WinRT. Nothing is installed; the folder is
# passed to CMake as WINAPPSDK_PACKAGES.
#
#   powershell -File fetch_packages.ps1 -Out C:\somewhere\packages
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$WindowsAppSdk = '2.6.4',
    [string]$CppWinRT = '3.0.260818.1'
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Out | Out-Null
$nuget = Join-Path $Out 'nuget.exe'
if (-not (Test-Path $nuget))
{
    Invoke-WebRequest https://dist.nuget.org/win-x86-commandline/latest/nuget.exe -OutFile $nuget
}
& $nuget install Microsoft.WindowsAppSDK -Version $WindowsAppSdk -OutputDirectory $Out -NonInteractive
& $nuget install Microsoft.Windows.CppWinRT -Version $CppWinRT -OutputDirectory $Out -NonInteractive
