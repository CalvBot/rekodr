# Saves the last 30 seconds as a clip over the pipe, then prints the clip's path once it is written.
$pipe = New-Object System.IO.Pipes.NamedPipeClientStream ".", "rekodr", "InOut"
$pipe.Connect(2000)
$reader = New-Object System.IO.StreamReader $pipe
$writer = New-Object System.IO.StreamWriter $pipe
$writer.AutoFlush = $true
$writer.WriteLine("save 30s")
while (($line = $reader.ReadLine()) -ne $null) {
    $msg = $line | ConvertFrom-Json
    if ($msg.type -eq "saved") {
        if ($msg.ok) { $msg.file } else { "Couldn't save: " + $msg.error }
        break
    }
}
$pipe.Dispose()
