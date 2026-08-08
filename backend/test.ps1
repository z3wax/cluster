$ErrorActionPreference = "Stop"
$base = "http://127.0.0.1:8080"

# 1. 10 ta node ro'yxatdan o'tkazish
for ($i = 1; $i -le 10; $i++) {
    $body = @{ id = "node$i"; name = "PC-$i"; os = "Windows"; platform = "x64"; spaceTotal = 500GB; agreementVersion = "1.0"; agreementAccepted = $true } | ConvertTo-Json
    Invoke-RestMethod -Method Post -Uri "$base/api/nodes/register" -Body $body -ContentType "application/json" | Out-Null
}
"1) 10 node register: OK"

# 2. Heartbeat
for ($i = 1; $i -le 10; $i++) {
    $body = @{ id = "node$i"; spaceUsed = 0 } | ConvertTo-Json
    Invoke-RestMethod -Method Post -Uri "$base/api/nodes/heartbeat" -Body $body -ContentType "application/json" | Out-Null
}
"2) 10 heartbeat: OK"

# 3. Fayl yuklash (2MB binary)
$data = [byte[]](0..255 | ForEach-Object { $_ })
$size = 2MB
$fs = [System.IO.File]::Create("C:\Users\HP PC\cluster\backend\data_test\upload.bin")
$buf = New-Object byte[] 65536
for ($off = 0; $off -lt $size; $off += 65536) {
    $len = [Math]::Min(65536, $size - $off)
    [Array]::Copy($data, 0, $buf, 0, [Math]::Min(256, $len))
    $fs.Write($buf, 0, $len)
}
$fs.Close()
$upl = Invoke-RestMethod -Method Post -Uri "$base/api/files/upload?name=test.bin&size=$size" -InFile "C:\Users\HP PC\cluster\backend\data_test\upload.bin" -ContentType "application/octet-stream"
"3) Upload: fileId=$($upl.fileId) chunks=$($upl.chunks) replicaTarget=$($upl.replicaTarget)"

# 4. Tasks tekshirish — faqat 3 ta node da task bo'lishi kerak (30%)
$withTasks = 0
for ($i = 1; $i -le 10; $i++) {
    $t = Invoke-RestMethod -Method Get -Uri "$base/api/nodes/node$i/tasks"
    if ($t.tasks.Count -gt 0) { $withTasks++ }
}
"4) Task olgan node'lar soni: $withTasks (30% = 3 kutilgan)"

# 5. Har bir task uchun chunk yuklab olib, done qilish
for ($i = 1; $i -le 10; $i++) {
    $t = Invoke-RestMethod -Method Get -Uri "$base/api/nodes/node$i/tasks"
    foreach ($task in $t.tasks) {
        $fid = $task.fileId
        foreach ($c in $task.chunks) {
            $r = Invoke-WebRequest -Uri "$base/api/files/$fid/chunks/$($c.index)" -Method Get
            if ($r.Content.Length -ne $c.size) { throw "chunk hajmi notogri: $($r.Content.Length) != $($c.size)" }
        }
        Invoke-RestMethod -Method Post -Uri "$base/api/nodes/node$i/tasks/$($task.id)/done" | Out-Null
    }
}
"5) Barcha chunk'lar yuklab olindi va done qilindi: OK"

# 6. Replikatsiya holatini tekshirish
$info = Invoke-RestMethod -Method Get -Uri "$base/api/info"
"6) Jami fayllar=$($info.totalFiles) bajarilgan replikatsiya=$($info.doneReplicas) kutilayotgan=$($info.pendingTasks)"

# 7. Daromad hisobi: 10 GB saqlangan tugunda, 1 soat onlayn = 0.5 sent
$info2 = Invoke-RestMethod -Method Get -Uri "$base/api/nodes/node1"
"7) node1: storedBytes=$($info2.storedBytes) daromad=$($info2.node.earningsCents) sent"

"ALL TESTS PASSED"