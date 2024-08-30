
<#
# Copyright (c) Microsoft Corporation
# SPDX-License-Identifier: MIT

.SYNOPSIS
This script provides helpers to install or uninstall the Net Event eBPF Extension (neteventebpfext) service.

.PARAMETER Action
    Specifies the action to take. This MUST be either "Install" or "Uninstall".

.PARAMETER BinaryDirectory
    Specifies the directory containing the necessary binaries (wtc.sys and bpfexport.exe).
    This MUST be the full path to the directory. By default, the current directory is used.

.EXAMPLE
    install_neteventebpfext.ps1 -Action Install

.EXAMPLE
    install_neteventebpfext.ps1 -Action Uninstall

.EXAMPLE
    install_neteventebpfext.ps1 -Action Install -BinaryDirectory "C:\binaries"
#>

param (
    [Parameter(Mandatory=$true)]
    [ValidateSet("Install", "Uninstall")]
    [string]$Action,
    [Parameter(Mandatory=$false)]
    [string]$BinaryDirectory = (Get-Location).Path
)

Set-StrictMode -Version 'Latest'
$PSDefaultParameterValues['*:ErrorAction'] = 'Stop'
$RootDir = Split-Path $PSScriptRoot -Leaf

$global:NetEventEBPFExtPath = $BinaryDirectory + "\neteventebpfext.sys"
$global:NetEventEBPFExtServiceName = "neteventebpfext"
$global:BpfExportPath = $BinaryDirectory + "\netevent_ebpf_ext_export_program_info.exe"


<#
.SYNOPSIS
Stops and deletes a service.

.PARAMETER ServiceName
The name of the service to cleanup.

.EXAMPLE
Cleanup-Serbvice -ServiceName "wtc"
#>
function Cleanup-Service(
    [Parameter(Mandatory=$true)]
    [string]$ServiceName
) {
    # Wait for the service to stop.
    $StopSuccess = $false
    try {
        Stop-Service $ServiceName
        for ($i = 0; $i -lt 100; $i++) {
            if (-not (Get-Service $ServiceName -ErrorAction Ignore) -or
                (Get-Service $ServiceName).Status -eq "Stopped") {
                $StopSuccess = $true
                break;
            }
            Start-Sleep -Milliseconds 100
        }
        if (!$StopSuccess) {
            Write-Verbose "$ServiceName failed to stop"
        }
    } catch {
        Write-Verbose "Exception while waiting for $ServiceName to stop"
    }

    # Delete the service.
    if (Get-Service $ServiceName -ErrorAction Ignore) {
        try { sc.exe delete $ServiceName > $null }
        catch { Write-Verbose "'sc.exe delete $ServiceName' threw exception!" }

        # Wait for the service to be deleted.
        $DeleteSuccess = $false
        for ($i = 0; $i -lt 10; $i++) {
            if (-not (Get-Service $ServiceName -ErrorAction Ignore)) {
                $DeleteSuccess = $true
                break;
            }
            Start-Sleep -Milliseconds 10
        }
        if (!$DeleteSuccess) {
            Write-Verbose "Failed to clean up $ServiceName!"
        }
    }
}

<#
.SYNOPSIS
Starts a service with retry attempts.

.PARAMETER ServiceName
The name of the service to start.

.EXAMPLE
Start-Service-With-Retry -ServiceName "wtc"
#>
function Start-Service-With-Retry(
    [Parameter(Mandatory=$true)]
    [string]$ServiceName
) {
    Write-Verbose "Start-Service $ServiceName"
    $StartSuccess = $false

    for ($i=0; $i -lt 100; $i++) {
        try {
            Start-Sleep -Milliseconds 10
            Start-Service $ServiceName
            $StartSuccess = $true
            break
        } catch { }
    }

    if ($StartSuccess -eq $false) {
        Write-Error "Failed to start $ServiceName"
    }
}

<#
.SYNOPSIS
Installs a service and starts it.

.PARAMETER ServiceName
The name of the service to install.

.PARAMETER BinaryPath
The path to the binary to install as a service. This MUST be the full path.

.EXAMPLE
Install-Service -ServiceName "wtc" -BinaryPath "C:\wtc.sys"
#>
function Install-Service(
    [Parameter(Mandatory=$true)]
    [string]$ServiceName,
    [Parameter(Mandatory=$true)]
    [string]$BinaryPath
)
{
    # Cleanup service if it already exists.
    $ServiceExists = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    if ($ServiceExists) {
        Write-Verbose "$ServiceName already exists. Attempting to cleanup the service first."
        Cleanup-Service -ServiceName $ServiceName
    }

    # Install the service.
    sc.exe create $ServiceName type= kernel binpath= $BinaryPath start= system | Write-Verbose
    if ($LastExitCode) { Write-Error "Failed to install driver" }

    # Start the service.
    Start-Service-With-Retry $ServiceName

    Write-Verbose "$ServiceName install complete!"
}

<#
.SYNOPSIS
This function installs the neteventebpfext service and updates the eBPF store.
#>
function Install-Neteventebpfext {
    Write-Verbose "Installing $global:NeteventebpfextServiceName at $global:NeteventebpfextPath"
    Install-Service -ServiceName $global:NeteventebpfextServiceName -BinaryPath $global:NeteventebpfextPath

    Write-Verbose "Updating eBPF store"
    & $global:BpfExportPath | Write-Verbose
    if ($LastExitCode) { Write-Error "Failed to update eBPF store" }

    Write-Verbose "Installation Complete!"
}


<#
.SYNOPSIS
This function uninstalls the neteventebpfext service and clears the eBPF store.
#>
function Uninstall-Neteventebpfext {
    # Stop and delete the service.
    Cleanup-Service -ServiceName $global:NeteventebpfextServiceName

    Write-Verbose "Clearing the eBPF store"
    & $global:BpfExportPath --clear | Write-Verbose
    if ($LastExitCode) { Write-Verbose "Failed to clear eBPF store" }

    Write-Verbose "Uninstall complete!"
}

if ($Action -eq "Install") {
    Install-Neteventebpfext
} elseif ($Action -eq "Uninstall") {
    Uninstall-Neteventebpfext
}