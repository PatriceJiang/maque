/* Maque .scn 预览扩展：调用 maque_player 把场景渲染成 PNG，在 WebView 里显示。
 *
 * 与仓库的关系：本扩展是**本地开发工具**（不走 marketplace）——它只做两件事：
 *   ① 找到 maque_player（设置 maque.playerPath → 工作区 build/samples/player → PATH）
 *   ② 渲染：`maque_player --shot <tmp.png> --shot-frame N <scene.scn>`（MAQUE_PRESENT=off，纯离屏）
 * 因此插件本身不含任何渲染逻辑：截图能力属于引擎（docs/AGENTS 的 `--shot`）。
 */
const vscode = require('vscode')
const { spawn } = require('child_process')
const crypto = require('crypto')
const fs = require('fs')
const os = require('os')
const path = require('path')

let g_output = null
let g_panel = null
let g_panelScene = null

function out() {
  if (!g_output) g_output = vscode.window.createOutputChannel('Maque')
  return g_output
}

function activate(context) {
  context.subscriptions.push(
    vscode.commands.registerCommand('maque.previewScn', (uri) => preview(uri)),
    vscode.commands.registerCommand('maque.setPlayerPath', () => setPlayerPath())
  )
}

/* ------------------------------- 找 player ------------------------------- */

async function findPlayer() {
  const cfg = vscode.workspace.getConfiguration('maque').get('playerPath')
  if (cfg && fs.existsSync(cfg)) return cfg
  for (const folder of vscode.workspace.workspaceFolders || []) {
    const guess = path.join(folder.uri.fsPath, 'build', 'samples', 'player', 'maque_player')
    if (fs.existsSync(guess)) return guess
  }
  for (const dir of (process.env.PATH || '').split(path.delimiter)) {
    const guess = path.join(dir, 'maque_player')
    if (fs.existsSync(guess)) return guess
  }
  return null
}

async function setPlayerPath() {
  const picked = await vscode.window.showOpenDialog({
    canSelectMany: false,
    openLabel: '选 maque_player',
    defaultUri: vscode.workspace.workspaceFolders?.[0]?.uri
  })
  if (!picked || picked.length === 0) return
  await vscode.workspace.getConfiguration('maque').update('playerPath', picked[0].fsPath,
    vscode.ConfigurationTarget.Workspace)
  vscode.window.showInformationMessage(`Maque: player 已设为 ${picked[0].fsPath}`)
}

/* ------------------------------- 渲染 ------------------------------- */

function renderPlayer(player, scene, outPath, frame, extraArgs, extraEnv) {
  return new Promise((resolve, reject) => {
    const args = ['--shot', outPath, '--shot-frame', String(frame), ...extraArgs, scene]
    const env = { ...process.env, MAQUE_PRESENT: 'off', ...extraEnv }
    out().appendLine(`$ ${player} ${args.join(' ')}`)
    const child = spawn(player, args, { env, cwd: path.dirname(scene) })
    let log = ''
    child.stdout.on('data', (d) => { log += d; out().append(d.toString()) })
    child.stderr.on('data', (d) => { log += d; out().append(d.toString()) })
    child.on('error', (err) => reject(new Error(`${player} 启动失败：${err.message}`)))
    child.on('close', (code) => {
      if (code === 0 && fs.existsSync(outPath)) resolve(log)
      else reject(new Error(`渲染失败（exit=${code}）\n${log.trim().split('\n').slice(-6).join('\n')}`))
    })
  })
}

async function preview(uri) {
  const scene = (uri && uri.fsPath) || vscode.window.activeTextEditor?.document.uri.fsPath
  if (!scene || !scene.toLowerCase().endsWith('.scn')) {
    vscode.window.showWarningMessage('Maque: 请对 .scn 文件使用（右键 → 预览场景）')
    return
  }
  const player = await findPlayer()
  if (!player) {
    const pick = await vscode.window.showErrorMessage(
      'Maque: 找不到 maque_player —— 先构建（cmake --build build），或用命令 “Maque: 指定 maque_player 路径”。',
      '立即指定')
    if (pick) await setPlayerPath()
    return
  }
  const cfg = vscode.workspace.getConfiguration('maque')
  const frame = Math.max(1, Number(cfg.get('shotFrame')) || 1)
  const extraArgs = cfg.get('shotArgs') || []
  const extraEnv = cfg.get('env') || {}
  const outPath = path.join(os.tmpdir(), `maque-scn-${crypto.createHash('sha1').update(scene).digest('hex').slice(0, 12)}.png`)

  try {
    await fs.promises.rm(outPath, { force: true })
    await vscode.window.withProgress(
      { location: vscode.ProgressLocation.Notification, title: `Maque: 渲染 ${path.basename(scene)}（第 ${frame} 帧）…`, cancellable: false },
      () => renderPlayer(player, scene, outPath, frame, extraArgs, extraEnv))
    await showPanel(scene, frame, await fs.promises.readFile(outPath))
  } catch (err) {
    const pick = await vscode.window.showErrorMessage(`Maque: ${err.message}`, '查看日志')
    if (pick) out().show()
  }
}

/* ------------------------------- WebView ------------------------------- */

function pngSize(buf) {
  if (buf.length < 24 || buf.readUInt32BE(0) !== 0x89504e47) return null
  return { w: buf.readUInt32BE(16), h: buf.readUInt32BE(20) }
}

function panelHtml(scene, frame, png) {
  const size = pngSize(png)
  const b64 = png.toString('base64')
  const checker = 'background-image:linear-gradient(45deg,#2b2b2b 25%,transparent 25%),linear-gradient(-45deg,#2b2b2b 25%,transparent 25%),linear-gradient(45deg,transparent 75%,#2b2b2b 75%),linear-gradient(-45deg,transparent 75%,#2b2b2b 75%);background-size:16px 16px;background-position:0 0,0 8px,8px -8px,-8px 0px;background-color:#1e1e1e;'
  return `<!doctype html><html><head><meta charset="utf-8">
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; img-src data:; style-src 'unsafe-inline'; script-src 'unsafe-inline';">
<style>
  body { margin: 0; padding: 12px; font: 12px/1.5 var(--vscode-font-family); color: var(--vscode-foreground); }
  .bar { display: flex; align-items: center; gap: 12px; margin-bottom: 8px; flex-wrap: wrap; }
  button { font: inherit; padding: 3px 10px; cursor: pointer; }
  .meta { opacity: .75; }
  .stage { ${checker} padding: 8px; border: 1px solid #333; display: inline-block; max-width: 100%; overflow: auto; }
  img { image-rendering: pixelated; display: block; max-width: 100%; }
</style></head><body>
<div class="bar">
  <button id="refresh">刷新</button>
  <button id="open">打开 .scn</button>
  <span class="meta">${scene}</span>
  <span class="meta">frame=${frame}${size ? ` · ${size.w}×${size.h}` : ''}</span>
</div>
<div class="stage"><img src="data:image/png;base64,${b64}" alt="scene preview"></div>
<script>
  const vscode = acquireVsCodeApi();
  document.getElementById('refresh').onclick = () => vscode.postMessage({ cmd: 'refresh' });
  document.getElementById('open').onclick = () => vscode.postMessage({ cmd: 'open' });
</script>
</body></html>`
}

async function showPanel(scene, frame, png) {
  if (!g_panel || g_panelScene !== scene) {
    if (g_panel) g_panel.dispose()
    g_panelScene = scene
    g_panel = vscode.window.createWebviewPanel('maqueScnPreview', `Maque 预览 · ${path.basename(scene)}`,
      vscode.ViewColumn.Beside, { enableScripts: true, retainContextWhenHidden: true })
    g_panel.onDidDispose(() => { g_panel = null; g_panelScene = null })
    g_panel.webview.onDidReceiveMessage((msg) => {
      if (msg.cmd === 'refresh') preview(vscode.Uri.file(scene))
      if (msg.cmd === 'open') vscode.window.showTextDocument(vscode.Uri.file(scene), { viewColumn: vscode.ViewColumn.One })
    })
  }
  g_panel.webview.html = panelHtml(scene, frame, png)
  g_panel.reveal(vscode.ViewColumn.Beside, true)
}

function deactivate() {
  if (g_panel) g_panel.dispose()
  if (g_output) g_output.dispose()
}

module.exports = { activate, deactivate }
