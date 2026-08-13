//---------------------------------------------------------------------------
// appserve ベースアプリ: ローカルファイルブラウザ
//
// フレームワークの使い方の見本も兼ねている:
//   - app.get()/post() で C++ 側 API を叩く
//   - app.command() で REPL (.b call ...) から操作できるようにする
//   - app.exposeState() で REPL (.b state) から状態を観測できるようにする
//---------------------------------------------------------------------------
import { app } from './lib/appserve.js';

const $ = (sel) => document.querySelector(sel);

const state = {
	cwd: null,
	entries: [],
	selected: null,
	sort: { key: 'name', asc: true },
	expanded: new Set(),
};

//---------------------------------------------------------------------------
// 表示ヘルパ
//---------------------------------------------------------------------------
function formatSize(n) {
	if (!n) return '';
	const u = ['B', 'KB', 'MB', 'GB', 'TB'];
	let i = 0, v = n;
	while (v >= 1024 && i < u.length - 1) { v /= 1024; ++i; }
	return (i === 0 ? v : v.toFixed(1)) + ' ' + u[i];
}

function formatTime(ms) {
	if (!ms) return '';
	// C++ 側の file_time_type は実装依存のエポックなので、値が明らかに
	// Unix エポックでない場合は表示しない (誤った日付を出すより無難)
	const d = new Date(ms);
	const y = d.getFullYear();
	if (y < 1990 || y > 2200) return '';
	const p = (x) => String(x).padStart(2, '0');
	return `${y}-${p(d.getMonth() + 1)}-${p(d.getDate())} ${p(d.getHours())}:${p(d.getMinutes())}`;
}

function setStatus(text, isError) {
	const el = $('#listStatus');
	el.textContent = text || '';
	el.classList.toggle('error', !!isError);
}

//---------------------------------------------------------------------------
// ディレクトリを開く
//---------------------------------------------------------------------------
async function openDir(path, { pushCrumbs = true } = {}) {
	try {
		setStatus('読み込み中…');
		const r = await app.get('/api/fs/list', { path });
		state.cwd = r.path;
		state.entries = r.entries || [];
		state.selected = null;
		$('#pathInput').value = r.path;
		renderList();
		if (pushCrumbs) renderCrumbs(r.path);
		setStatus(`${state.entries.length} 項目`);
		clearPreview();
	} catch (e) {
		setStatus(e.message, true);
	}
}

function sortEntries(list) {
	const { key, asc } = state.sort;
	const dir = asc ? 1 : -1;
	return list.slice().sort((a, b) => {
		// ディレクトリは常に先頭
		if (a.dir !== b.dir) return a.dir ? -1 : 1;
		let av = a[key], bv = b[key];
		if (key === 'name') {
			return dir * String(av).localeCompare(String(bv), 'ja', { numeric: true });
		}
		return dir * ((av || 0) - (bv || 0));
	});
}

function renderList() {
	const body = $('#listBody');
	body.textContent = '';
	const frag = document.createDocumentFragment();

	for (const e of sortEntries(state.entries)) {
		const row = document.createElement('div');
		row.className = 'row' + (e.dir ? ' dir' : '');
		row.dataset.path = e.path;

		const name = document.createElement('span');
		name.className = 'name';
		name.textContent = (e.dir ? '📁 ' : '📄 ') + e.name;
		const size = document.createElement('span');
		size.className = 'size';
		size.textContent = e.dir ? '' : formatSize(e.size);
		const mtime = document.createElement('span');
		mtime.className = 'mtime';
		mtime.textContent = formatTime(e.mtime);

		row.append(name, size, mtime);
		row.addEventListener('click', () => selectEntry(e, row));
		row.addEventListener('dblclick', () => {
			if (e.dir) openDir(e.path);
		});
		frag.appendChild(row);
	}
	body.appendChild(frag);
}

function selectEntry(e, row) {
	document.querySelectorAll('.row.sel').forEach(r => r.classList.remove('sel'));
	if (row) row.classList.add('sel');
	state.selected = e;
	if (e.dir) showDirMeta(e);
	else showPreview(e);
}

//---------------------------------------------------------------------------
// パンくず
//---------------------------------------------------------------------------
function renderCrumbs(path) {
	const el = $('#crumbs');
	el.textContent = '';
	const parts = path.split('/').filter(Boolean);
	let acc = path.startsWith('/') ? '' : null;

	parts.forEach((p, i) => {
		if (i > 0) {
			const sep = document.createElement('span');
			sep.className = 'sep';
			sep.textContent = '/';
			el.appendChild(sep);
		}
		acc = (acc === null) ? p + '/' : acc + '/' + p;
		const target = acc;
		const a = document.createElement('a');
		a.href = '#';
		a.textContent = p;
		a.addEventListener('click', (ev) => { ev.preventDefault(); openDir(target); });
		el.appendChild(a);
	});
}

//---------------------------------------------------------------------------
// プレビュー
//---------------------------------------------------------------------------
function clearPreview() {
	$('#previewMeta').textContent = '';
	$('#previewBody').innerHTML = '<p class="hint">ファイルを選ぶと内容を表示します。</p>';
}

function renderMeta(rows) {
	const el = $('#previewMeta');
	el.textContent = '';
	for (const [k, v] of rows) {
		const d = document.createElement('div');
		const ks = document.createElement('span');
		ks.className = 'k';
		ks.textContent = k;
		const vs = document.createElement('span');
		vs.className = 'v';
		vs.textContent = v;
		d.append(ks, vs);
		el.appendChild(d);
	}
}

function showDirMeta(e) {
	renderMeta([['名前', e.name], ['種別', 'ディレクトリ'], ['パス', e.path]]);
	$('#previewBody').innerHTML = '<p class="hint">ダブルクリックで開きます。</p>';
}

const TEXT_EXT = new Set([
	'txt', 'md', 'json', 'js', 'mjs', 'ts', 'css', 'html', 'htm', 'xml', 'svg',
	'csv', 'tsv', 'ini', 'cfg', 'conf', 'log', 'yml', 'yaml', 'toml',
	'c', 'h', 'cpp', 'hpp', 'cc', 'cxx', 'cs', 'py', 'rb', 'go', 'rs', 'java',
	'sh', 'bat', 'ps1', 'cmake', 'tjs', 'ks', 'gitignore',
]);
const IMAGE_EXT = new Set(['png', 'jpg', 'jpeg', 'gif', 'webp', 'bmp', 'svg', 'ico']);

function extOf(name) {
	const dot = name.lastIndexOf('.');
	return dot < 0 ? '' : name.slice(dot + 1).toLowerCase();
}

async function showPreview(e) {
	const ext = extOf(e.name);
	renderMeta([
		['名前', e.name],
		['サイズ', formatSize(e.size) + ` (${e.size} バイト)`],
		['更新', formatTime(e.mtime) || '(不明)'],
		['パス', e.path],
	]);
	const body = $('#previewBody');
	body.textContent = '';

	if (IMAGE_EXT.has(ext)) {
		const img = document.createElement('img');
		// 画像はサーバから直接読ませる (トークンはクエリで渡す)
		img.src = app._url('/api/fs/read', { path: e.path, t: app.token });
		img.alt = e.name;
		body.appendChild(img);
		return;
	}

	if (TEXT_EXT.has(ext) || e.size < 4096) {
		try {
			const r = await app.get('/api/fs/text', { path: e.path, length: 256 * 1024 });
			const pre = document.createElement('pre');
			pre.textContent = r.text;
			if (r.encoding !== 'utf-8') {
				const note = document.createElement('p');
				note.className = 'hint';
				note.textContent = `(${r.encoding} として読みました)`;
				body.appendChild(note);
			}
			body.appendChild(pre);
			return;
		} catch (err) {
			body.innerHTML = `<p class="hint">読み取れませんでした: ${err.message}</p>`;
			return;
		}
	}

	// それ以外は先頭のみ hex ダンプ
	try {
		const buf = await app.bytes('/api/fs/read', { path: e.path, length: 512 });
		const bytes = new Uint8Array(buf);
		let out = '';
		for (let i = 0; i < bytes.length; i += 16) {
			const chunk = bytes.slice(i, i + 16);
			const hex = [...chunk].map(b => b.toString(16).padStart(2, '0')).join(' ');
			const asc = [...chunk].map(b => (b >= 32 && b < 127) ? String.fromCharCode(b) : '.').join('');
			out += i.toString(16).padStart(8, '0') + '  ' + hex.padEnd(47) + '  ' + asc + '\n';
		}
		const pre = document.createElement('div');
		pre.className = 'hex';
		pre.textContent = out + (e.size > 512 ? `\n… (先頭 512 / ${e.size} バイト)` : '');
		body.appendChild(pre);
	} catch (err) {
		body.innerHTML = `<p class="hint">読み取れませんでした: ${err.message}</p>`;
	}
}

//---------------------------------------------------------------------------
// 左ペイン (場所 + ツリー)
//---------------------------------------------------------------------------
async function loadRoots() {
	const r = await app.get('/api/fs/roots');
	const ul = $('#rootList');
	ul.textContent = '';
	for (const root of r.roots || []) {
		const li = document.createElement('li');
		li.textContent = root.label;
		li.title = root.path;
		li.addEventListener('click', () => {
			openDir(root.path);
			buildTree(root.path);
		});
		ul.appendChild(li);
	}
	return r.roots || [];
}

async function buildTree(path) {
	const host = $('#treeBody');
	host.textContent = '';
	host.appendChild(await makeTreeNode(path, path, 0));
}

async function makeTreeNode(path, label, depth) {
	const node = document.createElement('div');
	node.className = 'tree-node';

	const row = document.createElement('div');
	row.className = 'tree-row';
	row.style.paddingLeft = (depth * 12 + 6) + 'px';

	const arrow = document.createElement('span');
	arrow.className = 'tree-arrow';
	arrow.textContent = '▸';

	const text = document.createElement('span');
	text.className = 'tree-label';
	text.textContent = label;

	row.append(arrow, text);
	node.appendChild(row);

	const children = document.createElement('div');
	children.className = 'tree-children';
	node.appendChild(children);

	let loaded = false;
	arrow.addEventListener('click', async (ev) => {
		ev.stopPropagation();
		const open = node.classList.toggle('open');
		arrow.textContent = open ? '▾' : '▸';
		if (open && !loaded) {
			loaded = true;
			try {
				const r = await app.get('/api/fs/list', { path });
				const dirs = (r.entries || []).filter(e => e.dir)
					.sort((a, b) => a.name.localeCompare(b.name, 'ja', { numeric: true }));
				for (const d of dirs) {
					children.appendChild(await makeTreeNode(d.path, d.name, depth + 1));
				}
				if (!dirs.length) arrow.textContent = '·';
			} catch (e) {
				arrow.textContent = '!';
			}
		}
	});

	row.addEventListener('click', () => {
		document.querySelectorAll('.tree-row.sel').forEach(r => r.classList.remove('sel'));
		row.classList.add('sel');
		openDir(path);
	});

	return node;
}

//---------------------------------------------------------------------------
// ログパネル
//---------------------------------------------------------------------------
function setupLog() {
	const panel = $('#logPanel');
	const body  = $('#logBody');

	$('#logBtn').addEventListener('click', () => panel.classList.toggle('hidden'));
	$('#logClose').addEventListener('click', () => panel.classList.add('hidden'));

	app.on('log', (e) => {
		const line = document.createElement('div');
		line.className = e.level || 'info';
		line.textContent = e.text;
		body.appendChild(line);
		while (body.childElementCount > 500) body.removeChild(body.firstChild);
		body.scrollTop = body.scrollHeight;
	});
}

//---------------------------------------------------------------------------
// REPL からの操作口
//---------------------------------------------------------------------------
function setupReplBridge() {
	app.command('open',    (arg) => openDir(typeof arg === 'string' ? arg : arg.path));
	app.command('reload',  () => openDir(state.cwd));
	app.command('select',  (arg) => {
		const name = typeof arg === 'string' ? arg : arg.name;
		const e = state.entries.find(x => x.name === name || x.path === name);
		if (!e) throw new Error('no such entry: ' + name);
		const row = document.querySelector(`.row[data-path="${CSS.escape(e.path)}"]`);
		selectEntry(e, row);
		return e;
	});
	app.command('entries', () => state.entries.map(e => e.name));

	app.exposeState(() => ({
		cwd: state.cwd,
		count: state.entries.length,
		selected: state.selected ? state.selected.path : null,
		sort: state.sort,
	}));

	app.handler('nav', (arg) => openDir(arg.path));
}

//---------------------------------------------------------------------------
// 起動
//---------------------------------------------------------------------------
async function main() {
	const info = await app.ready();
	document.title = info.app + ' — ' + (state.cwd || '');
	$('#brand').textContent = info.app;

	setupLog();
	setupReplBridge();

	// 並び替え
	document.querySelectorAll('.list-head .col').forEach(col => {
		col.addEventListener('click', () => {
			const key = col.dataset.sort;
			if (state.sort.key === key) state.sort.asc = !state.sort.asc;
			else state.sort = { key, asc: true };
			renderList();
		});
	});

	$('#pathInput').addEventListener('keydown', (e) => {
		if (e.key === 'Enter') openDir($('#pathInput').value.trim());
	});
	$('#reloadBtn').addEventListener('click', () => openDir(state.cwd));

	const roots = await loadRoots();

	// --open / 位置引数で渡されたパスがあればそこを開く
	let start = null;
	try {
		const s = await app.get('/api/app/startup');
		if (s && s.open) start = s.open;
	} catch (e) { /* 派生アプリでは無いかもしれない */ }
	if (!start) {
		const cwd = roots.find(r => r.kind === 'cwd');
		start = cwd ? cwd.path : (roots[0] ? roots[0].path : '.');
	}

	await openDir(start);
	buildTree(start);
}

main().catch(e => {
	setStatus('起動に失敗しました: ' + e.message, true);
	console.error(e);
});
