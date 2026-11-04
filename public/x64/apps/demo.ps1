Write-Output ('PowerShell subset inside the x64 emulator: {0}' -f (Get-Date -Format 'yyyy-MM-dd'))
Get-ChildItem C:\apps -Name | Select-Object -First 8
(Get-FileHash C:\apps\jq.exe).Hash
& 'C:\apps\jq.exe' -n '{a:1}'
