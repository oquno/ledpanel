"""Run native browser ES-module tests using an installed Chromium browser, without Node."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import threading
import time
import json
from urllib.request import urlopen
from browser_cdp import BrowserSocket
from dev_server import create_server


def find_browser():
    candidates = [shutil.which(name) for name in ('chromium', 'chromium-browser', 'google-chrome', 'msedge')]
    for env in ('PROGRAMFILES', 'PROGRAMFILES(X86)', 'LOCALAPPDATA'):
        base = Path(os.environ.get(env, '.'))
        candidates.extend(str(base / name) for name in ('Microsoft/Edge/Application/msedge.exe', 'Google/Chrome/Application/chrome.exe'))
    return next((str(p) for p in candidates if p and Path(p).is_file()), None)


def wait_for_browser_port(port_file, process, timeout=15):
    deadline = time.monotonic() + timeout
    last_error = None
    while True:
        returncode = process.poll()
        if returncode is not None:
            raise RuntimeError(f'Browser exited during startup (exit code {returncode})') from last_error
        try:
            # Chromium may create the file before finishing its write. On
            # Windows the writer can also temporarily deny read access.
            port = int(port_file.read_text(encoding='utf-8').splitlines()[0])
            if not 1 <= port <= 65535:
                raise ValueError('Invalid browser debugging port')
            return port
        except (OSError, ValueError, IndexError) as error:
            last_error = error
        if time.monotonic() >= deadline:
            raise RuntimeError('Timed out waiting for browser debugging port') from last_error
        time.sleep(.05)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--browser', help='Path to Chrome, Edge, or Chromium')
    args = parser.parse_args()
    browser = args.browser or find_browser()
    if not browser:
        parser.error('No Chromium browser found. Pass --browser, or open /tests/browser/index.html on the development server.')
    with create_server(0) as server, tempfile.TemporaryDirectory(prefix='ledpanel-browser-') as profile:
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            process = subprocess.Popen([browser, '--headless', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
                                        '--disable-background-networking', '--remote-debugging-port=0', f'--user-data-dir={profile}', 'about:blank'],
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                       creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
            client = None
            try:
                port_file = Path(profile) / 'DevToolsActivePort'
                port = wait_for_browser_port(port_file, process)
                with urlopen(f'http://127.0.0.1:{port}/json/list') as response:
                    pages = json.load(response)
                client = BrowserSocket(next(p['webSocketDebuggerUrl'] for p in pages if p['type'] == 'page'))
                client.call('Page.navigate', url=f'http://127.0.0.1:{server.server_port}/tests/browser/index.html')
                deadline = time.monotonic() + 45
                while time.monotonic() < deadline:
                    result = client.call('Runtime.evaluate', expression='JSON.stringify({done:document.body?.dataset.done,failed:document.body?.dataset.failed,results:document.querySelector("#results")?.textContent})', returnByValue=True)
                    state = json.loads(result['result']['value'])
                    if state.get('done') == 'true':
                        print(state['results'])
                        return 0 if state['failed'] == '0' else 1
                    time.sleep(.05)
                print(state.get('results', 'Browser tests timed out'))
                return 1
            finally:
                if client:
                    client.close()
                process.terminate()
                process.wait(timeout=10)
        finally:
            server.shutdown()
            thread.join()


if __name__ == '__main__':
    raise SystemExit(main())
