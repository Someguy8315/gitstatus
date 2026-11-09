// End-to-end tests for the daemon protocol. Run after ./build -w with:
// node --test tests/gitstatus.test.js
import assert from 'node:assert/strict';
import { spawn, execFileSync } from 'node:child_process';
import { once } from 'node:events';
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, symlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';
import { test } from 'node:test';

const binary = resolve(process.env.GITSTATUS_TEST_DAEMON || 'usrbin/gitstatusd');
const fields = ['id', 'ok', 'workdir', 'commit', 'branch', 'remoteBranch', 'remote',
  'remoteUrl', 'action', 'index', 'staged', 'unstaged', 'conflicted', 'untracked',
  'ahead', 'behind', 'stashes', 'tag', 'unstagedDeleted', 'stagedNew', 'stagedDeleted',
  'pushRemote', 'pushUrl', 'pushAhead', 'pushBehind', 'skipWorktree', 'assumeUnchanged',
  'encoding', 'summary'];

for (const threads of [1, 8]) {
  test(`daemon status and cache refresh (${threads} threads)`, { timeout: 60000 }, async (t) => {
    const root = mkdtempSync(join(tmpdir(), 'gitstatus-test-'));
    const env = { ...process.env, HOME: root, XDG_CONFIG_HOME: root,
      GIT_CONFIG_NOSYSTEM: '1', GIT_CONFIG_GLOBAL: '/dev/null',
      GIT_AUTHOR_NAME: 'Test', GIT_AUTHOR_EMAIL: 'test@example.com',
      GIT_COMMITTER_NAME: 'Test', GIT_COMMITTER_EMAIL: 'test@example.com' };
    // Do not let the caller's Git environment select a different repository/index.
    for (const key of ['GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR']) delete env[key];
    const repo = join(root, 'repo');
    mkdirSync(repo);
    const git = (...args) => execFileSync('git', ['-C', repo, ...args], { env, encoding: 'utf8', stdio: ['ignore', 'pipe', 'pipe'] }).trim();
    const put = (path, text = 'contents\n') => writeFileSync(join(repo, path), text);
    const daemon = spawn(binary, ['-t', String(threads), '-s', '-1', '-u', '-1', '-c', '-1', '-d', '-1'], { env });
    let log = '';
    let buffer = '';
    let pending;
    daemon.stderr.on('data', data => { log += data; });
    daemon.stdout.on('data', data => {
      buffer += data;
      const end = buffer.indexOf('\x1e');
      if (end < 0) return;
      const record = buffer.slice(0, end).split('\x1f');
      buffer = buffer.slice(end + 1);
      const waiter = pending;
      pending = undefined;
      waiter?.resolve(Object.fromEntries(fields.map((name, i) => [name, record[i]])));
    });
    daemon.on('exit', code => pending?.reject(new Error(`daemon exited ${code}\n${log}`)));
    daemon.on('error', error => pending?.reject(error));
    t.after(async () => {
      if (daemon.exitCode === null) {
        const exited = once(daemon, 'exit');
        daemon.stdin.end();
        await exited;
      }
      rmSync(root, { recursive: true, force: true });
    });
    let serial = 0;
    const oneShot = (args = [], dir = repo) => {
      const output = execFileSync(binary, ['-t', String(threads), ...args], {
        env, input: `once\x1f${dir}\x1e`, encoding: 'utf8', timeout: 10000,
        stdio: ['pipe', 'pipe', 'pipe'],
      }).replace(/\x1e$/, '').split('\x1f');
      return Object.fromEntries(fields.map((name, i) => [name, output[i]]));
    };
    const query = async (dir = repo, expected = {}) => {
      const id = String(++serial);
      const response = await new Promise((resolve, reject) => {
        const timer = setTimeout(() => reject(new Error(`query timed out\n${log}`)), 10000);
        pending = { resolve: value => { clearTimeout(timer); resolve(value); }, reject: error => { clearTimeout(timer); reject(error); } };
        daemon.stdin.write(`${id}\x1f${dir}\x1e`);
      });
      assert.equal(response.id, id);
      for (const [key, value] of Object.entries(expected)) assert.equal(response[key], String(value), `${key}: ${JSON.stringify(response)}\n${log}`);
      return response;
    };

    await query(root, { ok: 0 });
    git('init', '-b', 'main');
    await query(repo, { ok: 1, branch: 'main', commit: '', index: 0, staged: 0, untracked: 0 });
    put('tracked');
    git('add', '.');
    await query(repo, { staged: 1, stagedNew: 1 });
    git('commit', '-m', 'initial');
    const initial = git('rev-parse', 'HEAD');
    await query(repo, { commit: initial, summary: 'initial', staged: 0, unstaged: 0, untracked: 0 });

    put('tracked', 'changed\n');
    put('new');
    await query(repo, { staged: 0, unstaged: 1, untracked: 1 });
    git('add', '.');
    await query(repo, { staged: 2, stagedNew: 1, unstaged: 0, untracked: 0 });
    git('commit', '-m', 'second');
    git('tag', '-a', 'v-test', '-m', 'tag');
    git('pack-refs', '--all');
    await query(repo, { staged: 0, tag: 'v-test' });
    git('tag', '-d', 'v-test');
    await query(repo, { tag: '' });

    git('remote', 'add', 'origin', 'https://example.com/test.git');
    git('update-ref', 'refs/remotes/origin/main', initial);
    git('config', 'branch.main.remote', 'origin');
    git('config', 'branch.main.merge', 'refs/heads/main');
    git('config', 'branch.main.pushRemote', 'origin');
    await query(repo, { remote: 'origin', remoteBranch: 'main', ahead: 1, behind: 0, pushRemote: 'origin', pushAhead: 1 });
    git('config', 'remote.origin.url', 'https://example.com/changed.git');
    await query(repo, { remoteUrl: 'https://example.com/changed.git', pushUrl: 'https://example.com/changed.git' });
    git('config', '--unset', 'branch.main.pushRemote');
    git('config', 'remote.pushDefault', 'origin');
    await query(repo, { pushRemote: 'origin', pushAhead: 1 });
    git('config', 'branch.main.remote', '.');
    git('config', 'branch.main.pushRemote', '.');
    await query(repo, { remote: '.', remoteBranch: 'main', remoteUrl: '', ahead: 0, behind: 0,
      pushRemote: '.', pushUrl: '', pushAhead: 0, pushBehind: 0 });
    git('config', 'branch.main.remote', 'origin');
    git('config', '--unset', 'branch.main.pushRemote');

    put('.gitignore', '*.ignored\n!keep.ignored\n');
    git('add', '.gitignore');
    git('commit', '-m', 'ignores');
    put('drop.ignored');
    put('keep.ignored');
    await query(repo, { untracked: 1 });
    git('config', 'status.showUntrackedFiles', 'false');
    await query(repo, { untracked: 0 });
    git('config', 'status.showUntrackedFiles', 'true');
    await query(repo, { untracked: 1 });
    rmSync(join(repo, 'keep.ignored'));

    mkdirSync(join(repo, 'nested'));
    put('nested/.gitignore', '!unignored.ignored\n');
    git('add', 'nested/.gitignore');
    git('commit', '-m', 'nested ignores');
    put('nested/unignored.ignored');
    assert.equal(git('status', '--porcelain'), '?? nested/unignored.ignored');
    await query(repo, { untracked: 1 });
    rmSync(join(repo, 'nested/unignored.ignored'));
    mkdirSync(join(repo, 'empty'));
    mkdirSync(join(repo, 'ignored-only'));
    put('ignored-only/drop.ignored');
    await query(repo, { untracked: 0 });

    // Notify callbacks must see final typechange classifications, not provisional deltas.
    rmSync(join(repo, 'tracked'));
    mkdirSync(join(repo, 'tracked'));
    put('tracked/child');
    await query(repo, { unstaged: 1, unstagedDeleted: 0, untracked: 0 });
    rmSync(join(repo, 'tracked'), { recursive: true });
    git('restore', 'tracked');
    rmSync(join(repo, 'nested'), { recursive: true });
    put('nested');
    await query(repo, { unstaged: 1, unstagedDeleted: 1, untracked: 0 });
    rmSync(join(repo, 'nested'));
    git('restore', 'nested');

    mkdirSync(join(repo, 'b'));
    put('b/known');
    put('b.c');
    git('add', 'b', 'b.c');
    git('commit', '-m', 'range boundary');
    put('b/known', 'changed\n');
    put('b.c', 'changed\n');
    await query(repo, { unstaged: 2 });
    git('restore', 'b', 'b.c');

    put('intent');
    git('add', '-N', 'intent');
    await query(repo, { staged: 0, unstaged: 1, stagedNew: 0 });
    git('add', 'intent');
    await query(repo, { staged: 1, stagedNew: 1, unstaged: 0 });
    git('commit', '-m', 'intent');
    git('update-index', '--skip-worktree', 'intent');
    git('update-index', '--assume-unchanged', 'new');
    await query(repo, { skipWorktree: 1, assumeUnchanged: 1 });
    git('update-index', '--no-skip-worktree', 'intent');
    git('update-index', '--no-assume-unchanged', 'new');
    git('update-index', '--index-version', '4');
    await query(repo, { skipWorktree: 0, assumeUnchanged: 0, staged: 0 });

    rmSync(join(repo, 'tracked'));
    await query(repo, { unstaged: 1, unstagedDeleted: 1 });
    git('add', '-u');
    await query(repo, { staged: 1, stagedDeleted: 1, unstaged: 0 });
    git('restore', '--staged', 'tracked');
    git('restore', 'tracked');
    put('tracked', 'stash change\n');
    git('stash', 'push', '-m', 'test stash');
    await query(repo, { stashes: 1, unstaged: 0 });
    git('stash', 'drop');
    await query(repo, { stashes: 0 });

    const worktree = join(root, 'worktree');
    git('worktree', 'add', '-b', 'linked', worktree);
    await query(worktree, { ok: 1, branch: 'linked', workdir: worktree, staged: 0 });
    await query(`:${join(worktree, '.git')}`, { ok: 1, branch: 'linked', workdir: worktree });
    await query(`:${join(repo, '.git')}`, { ok: 1, branch: 'main', workdir: repo });

    const child = join(root, 'child');
    git('init', '-b', 'main', child);
    writeFileSync(join(child, 'child-file'), 'child\n');
    git('-C', child, 'add', '.');
    git('-C', child, 'commit', '-m', 'child');
    git('-c', 'protocol.file.allow=always', 'submodule', 'add', child, 'sub');
    git('commit', '-m', 'submodule');
    await query(repo, { staged: 0, unstaged: 0 });
    await query(join(repo, 'sub'), { ok: 1, branch: 'main', workdir: join(repo, 'sub') });
    await query(`:${join(repo, 'sub', '.git')}`, { ok: 1, branch: 'main', workdir: join(repo, 'sub') });
    writeFileSync(join(repo, 'sub', 'child-file'), 'dirty child\n');
    await query(repo, { unstaged: 0 }); // Submodule workdir dirt is intentionally ignored.
    git('-C', join(repo, 'sub'), 'restore', 'child-file');
    const link = join(root, 'link');
    symlinkSync(repo, link);
    await query(link, { ok: 1, branch: 'main', staged: 0 });
    git('checkout', '--detach');
    await query(repo, { branch: '', ok: 1 });
    git('checkout', 'main');

    // More than 512 paths in several directories exercises parallel diff shards.
    for (let d = 0; d < 4; ++d) {
      mkdirSync(join(repo, `dir${d}`));
      for (let i = 0; i < 300; ++i) put(`dir${d}/${String(i).padStart(3, '0')}`);
    }
    git('add', '.');
    git('commit', '-m', 'many files');
    git('tag', '-a', 'parallel-tag', '-m', 'packed annotated tag');
    git('pack-refs', '--all');
    git('repack', '-ad');
    await query(repo, { staged: 0, unstaged: 0, untracked: 0, tag: 'parallel-tag' });
    for (let d = 0; d < 4; ++d) {
      put(`dir${d}/100`, 'staged\n');
      git('add', `dir${d}/100`);
      put(`dir${d}/200`, 'unstaged\n');
      put(`dir${d}/untracked`);
    }
    for (let i = 0; i < 10; ++i) await query(repo, { staged: 4, unstaged: 4, untracked: 4 });
    const limited = oneShot();
    for (const key of ['staged', 'unstaged', 'untracked']) assert.equal(limited[key], '1', key);
    git('config', 'core.ignoreCase', 'true');
    const insensitive = oneShot(['-s', '-1', '-u', '-1', '-d', '-1']);
    for (const key of ['staged', 'unstaged', 'untracked']) assert.equal(insensitive[key], '4', key);
    git('config', 'core.ignoreCase', 'false');
    git('add', '.');
    git('commit', '-m', 'changes');

    git('checkout', '-b', 'other');
    put('tracked', 'other\n');
    git('commit', '-am', 'other');
    git('checkout', 'main');
    put('tracked', 'main\n');
    git('commit', '-am', 'main');
    assert.throws(() => git('merge', 'other'));
    await query(repo, { action: 'merge', conflicted: 1 });
    git('merge', '--abort');
    await query(repo, { action: '', conflicted: 0, staged: 0, unstaged: 0 });

    const shallow = join(root, 'shallow');
    git('clone', '--no-local', '--depth=1', '--branch=main', repo, shallow);
    await query(shallow, { ok: 1, branch: 'main', ahead: 0, behind: 0 });
    git('-C', shallow, 'commit', '--allow-empty', '-m', 'shallow commit');
    await query(shallow, { ahead: 1, behind: 0 });
    // These repository extensions no longer need the old fork's allowlist patch.
    git('-C', shallow, 'config', 'core.repositoryFormatVersion', '1');
    git('-C', shallow, 'config', 'extensions.preciousObjects', 'true');
    git('-C', shallow, 'config', 'extensions.noop', 'true');
    assert.equal(oneShot([], shallow).ok, '1');

    // A zero index checksum cannot identify index contents across requests.
    git('config', 'index.skipHash', 'true');
    put('skip-hash-1');
    git('add', 'skip-hash-1');
    await query(repo, { staged: 1, stagedNew: 1 });
    put('skip-hash-2');
    git('add', 'skip-hash-2');
    await query(repo, { staged: 2, stagedNew: 2 });
  });
}
