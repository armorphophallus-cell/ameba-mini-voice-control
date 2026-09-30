#pragma once

const char PAGE[] = R"HTML(<!doctype html>
<html lang="zh-Hant"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>AMB82 語音燈光控制</title>
<style>
body{font-family:system-ui,sans-serif;max-width:640px;margin:32px auto;padding:0 18px;background:#f4f7fb;color:#14213d}
.card{background:#fff;border-radius:16px;padding:22px;box-shadow:0 8px 28px #20305018}.row{display:flex;gap:16px;flex-wrap:wrap}
.item{flex:1;min-width:150px;background:#eef3f9;padding:14px;border-radius:12px}.lamp{display:inline-block;width:18px;height:18px;border-radius:50%;background:#9aa4b2;margin-right:8px}
.blue.on{background:#168cff;box-shadow:0 0 14px #168cff}.green.on{background:#22b455;box-shadow:0 0 14px #22b455}
#resultPanel{margin:18px 0;padding:20px;border-radius:14px;background:#eef3f9;border:2px solid transparent;text-align:center}
#resultPanel.success{background:#e9f8ef;border-color:#35a65f}#resultPanel.unknown{background:#fff3e0;border-color:#ef8c22}#resultPanel.timeout{background:#edf2f8;border-color:#7890a8}#resultPanel.error{background:#fdecec;border-color:#d63b3b}
#resultTitle{display:block;font-size:1.6rem;margin-bottom:6px}#resultDetail{margin:0}.badge{display:inline-block;padding:5px 10px;border-radius:999px;background:#dfe7f1;font-weight:700}
button{width:100%;margin:16px 0;padding:13px;border:0;border-radius:12px;background:#244b78;color:white;font-size:1rem;font-weight:700;cursor:pointer}button.paused{background:#18834b}button:disabled{opacity:.55}
#connection.ok{color:#167a39}#connection.bad{color:#c62828}small{color:#596579}
</style></head><body><div class="card">
<h1>語音燈光控制</h1><p id="connection">正在連線…</p>
<div id="resultPanel"><strong id="resultTitle">等待語音指令</strong><p id="resultDetail">請說「左邊開燈」或「右邊開燈」</p></div>
<button id="pauseButton" type="button">暫停語音辨識</button>
<div class="row"><div class="item">辨識結果<br><strong id="recognition" class="badge">尚無</strong></div><div class="item">信心分數<br><strong id="score">—</strong></div></div>
<p>指令執行情況：<strong id="executed">尚無指令</strong></p>
<div class="row"><div class="item"><i id="blueLamp" class="lamp blue"></i>左邊／藍燈：<strong id="blue">關</strong></div><div class="item"><i id="greenLamp" class="lamp green"></i>右邊／綠燈：<strong id="green">關</strong></div></div>
<p><small>LED 狀態來自開發板 GPIO 回讀。無法辨識、非控制指令或低於門檻時不改變 LED。</small></p>
</div><script>
let failures=0;
pauseButton.onclick=async()=>{pauseButton.disabled=true;try{const r=await fetch('/api/pause',{method:'POST'});if(!r.ok)throw Error();await refresh();}catch(e){connection.textContent='操作失敗：無法連線至開發板';connection.className='bad';}finally{pauseButton.disabled=false;}};
async function refresh(){try{const r=await fetch('/api/state',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);const s=await r.json();
connection.textContent='已連線至開發板';connection.className='ok';pauseButton.textContent=s.paused?'繼續語音辨識':'暫停語音辨識';pauseButton.className=s.paused?'paused':'';recognition.textContent=({left:'左邊',right:'右邊',unknown:'無法辨識',timeout:'10 秒無指令',paused:'辨識已暫停'})[s.recognition]||'尚無';score.textContent=s.score>=0?s.score:'—';
executed.textContent=s.recognition==='timeout'?'已自動關閉左右燈':(s.executed?'已執行 '+(s.recognition==='left'?'左邊藍燈':'右邊綠燈'):(s.sequence?'未執行，LED 維持原狀':'尚無指令'));
resultPanel.className='';if(s.recognition==='left'||s.recognition==='right'){resultPanel.classList.add('success');resultTitle.textContent=s.recognition==='left'?'辨識成功：左邊':'辨識成功：右邊';resultDetail.textContent=s.executed?'指令已執行，狀態已由 GPIO 確認':'指令處理中';}
else if(s.recognition==='unknown'&&s.sequence){resultPanel.classList.add('unknown');resultTitle.textContent='無法辨識';resultDetail.textContent='不是有效控制指令，LED 維持原狀';}
else if(s.recognition==='timeout'){resultPanel.classList.add('timeout');resultTitle.textContent='10 秒沒有有效指令';resultDetail.textContent='左右 LED 已自動關閉';}
else if(s.recognition==='paused'){resultPanel.classList.add('timeout');resultTitle.textContent='語音辨識已暫停';resultDetail.textContent='左右 LED 已關閉，按「繼續語音辨識」恢復';}
blue.textContent=s.blue?'開':'關';green.textContent=s.green?'開':'關';blueLamp.classList.toggle('on',s.blue);greenLamp.classList.toggle('on',s.green);failures=0;
}catch(e){if(++failures>=2){connection.textContent='通訊失敗：請確認仍連著 AMB82-Voice 熱點';connection.className='bad';resultPanel.className='error';resultTitle.textContent='通訊失敗';resultDetail.textContent='無法取得開發板回傳狀態';}}}
setInterval(refresh,700);refresh();
</script></body></html>)HTML";
