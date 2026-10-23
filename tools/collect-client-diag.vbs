' 放在 ministream-client.exe 同一目录，双击运行，会生成 诊断包.zip
' 用 cscript / wscript 都可以跑；纯 VBScript，不依赖 PowerShell 版本
Dim fso, shell, shExec, out, zipPath, files, i
Set fso = CreateObject("Scripting.FileSystemObject")
Set shell = CreateObject("WScript.Shell")

Dim dir : dir = fso.GetParentFolderName(WScript.ScriptFullName)
Dim tmp : tmp = dir & "\_diag_tmp"
If fso.FolderExists(tmp) Then fso.DeleteFolder tmp, True
fso.CreateFolder tmp

' 1) 拷贝日志和崩溃文件
For Each n In Array("client.log", "crash.log")
    If fso.FileExists(dir & "\" & n) Then fso.CopyFile dir & "\" & n, tmp & "\"
Next
For Each f In fso.GetFolder(dir).Files
    If Left(f.Name, 14) = "crash_client_" And Right(f.Name, 4) = ".dmp" Then
        f.Copy tmp & "\"
    End If
Next

' 2) 系统信息（Windows 版本 + 显卡 + 内存）
Set out = fso.CreateTextFile(tmp & "\system.txt", True)
out.WriteLine "=== winver / os ==="
Set shExec = shell.Exec("cmd /c ver")
out.WriteLine shExec.StdOut.ReadAll
Set shExec = shell.Exec("cmd /c wmic os get Caption,Version,OSArchitecture /format:list")
out.WriteLine shExec.StdOut.ReadAll
out.WriteLine "=== gpu ==="
Set shExec = shell.Exec("cmd /c wmic path win32_VideoController get Name,DriverVersion,AdapterRAM /format:list")
out.WriteLine shExec.StdOut.ReadAll
out.WriteLine "=== ram ==="
Set shExec = shell.Exec("cmd /c wmic computersystem get TotalPhysicalMemory /format:list")
out.WriteLine shExec.StdOut.ReadAll
out.Close

' 3) Windows 事件日志里 ministream-client 的崩溃记录
Set shExec = shell.Exec("powershell -NoProfile -Command ""Get-WinEvent -FilterHashtable @{LogName='Application'; Id=1000} -MaxEvents 200 | Where-Object { $_.Message -match 'ministream' } | Select-Object -First 5 TimeCreated, Message | Format-List""")
If fso.FileExists(tmp & "\events.txt") Then fso.DeleteFile tmp & "\events.txt"
Set out = fso.CreateTextFile(tmp & "\events.txt", True)
out.WriteLine shExec.StdOut.ReadAll
out.Close

' 4) 打包成 zip（Windows 自带）
zipPath = dir & "\诊断包.zip"
If fso.FileExists(zipPath) Then fso.DeleteFile zipPath
' 先建一个空 zip
Set out = fso.CreateTextFile(zipPath, True)
out.Write "PK" & Chr(5) & Chr(6) & String(18, Chr(0))
out.Close
Dim zipApp
Set zipApp = CreateObject("Shell.Application")
zipApp.NameSpace(zipPath).CopyHere zipApp.NameSpace(tmp).Items, 16
' 等复制完成
WScript.Sleep 3000

fso.DeleteFolder tmp, True
MsgBox "诊断信息已打包到：" & zipPath & vbCrLf & "请把这个文件发回来。", vbInformation, "jianhx 诊断"
