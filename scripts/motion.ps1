# =============================================================================
#  motion.ps1 - moving picture source for throughput tests
#
#  DXGI Desktop Duplication only delivers a frame when the screen CHANGES, so an
#  idle desktop measures 5-15 fps no matter how good the pipeline is. This opens
#  a small window that repaints at ~60 Hz so that capture/encode/send can be
#  measured under real motion.
#
#  Usage:  powershell -File scripts\motion.ps1 -Seconds 20
#  ASCII only.
# =============================================================================
param(
    [int]$Seconds = 20,
    [int]$Width   = 720,
    [int]$Height  = 420
)

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$form = New-Object System.Windows.Forms.Form
$form.Text = 'MiniStream motion source'
$form.ClientSize = New-Object System.Drawing.Size($Width, $Height)
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point(80, 80)
$form.TopMost = $true
$form.BackColor = [System.Drawing.Color]::FromArgb(16, 16, 20)

$global:msTick = 0
$fontBig   = New-Object System.Drawing.Font('Consolas', 34, [System.Drawing.FontStyle]::Bold)
$fontSmall = New-Object System.Drawing.Font('Consolas', 12)
$brushBar  = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(255, 150, 40))
$brushBar2 = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(60, 160, 255))
$brushText = [System.Drawing.Brushes]::White

$form.Add_Paint({
    param($sender, $ev)
    $g = $ev.Graphics
    $g.Clear([System.Drawing.Color]::FromArgb(16, 16, 20))
    $w = $sender.ClientSize.Width
    $h = $sender.ClientSize.Height
    $x = [int]($global:msTick * 3) % ([Math]::Max($w - 140, 1))
    $y = [int](60 + 90 * [Math]::Sin($global:msTick / 34.0))
    $g.FillRectangle($brushBar, $x, $y, 130, 130)
    $x2 = [int]($w - 140 - ($global:msTick * 4) % ([Math]::Max($w - 140, 1)))
    $g.FillRectangle($brushBar2, $x2, [int]($h - 190), 110, 110)
    $g.DrawString((Get-Date).ToString('HH:mm:ss.fff'), $fontBig, $brushText, 18, [int]($h - 150))
    $g.DrawString("frame $global:msTick", $fontSmall, $brushText, 18, 14)
    $g.Dispose()
})

$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 16
$timer.Add_Tick({
    $global:msTick++
    $form.Invalidate()
})

$timer.Start()
$form.Show()
[System.Windows.Forms.Application]::DoEvents()

$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end) {
    [System.Windows.Forms.Application]::DoEvents()
    Start-Sleep -Milliseconds 4
}

$timer.Stop()
$form.Close()
Write-Host "motion source finished ($global:msTick frames)"
