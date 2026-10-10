const cases = [];
export default function test(name, run) { cases.push({name, run}); }
function fail(message) { throw new Error(message); }
export const assert = {
  equal(a, b) { if (a !== b) fail(`Expected ${JSON.stringify(b)}, got ${JSON.stringify(a)}`); },
  deepEqual(a, b) { this.equal(JSON.stringify(a), JSON.stringify(b)); },
  ok(value) { if (!value) fail('Expected truthy value'); },
  async rejects(run) { try { await run(); } catch { return; } fail('Expected rejection'); },
};
export async function run() {
  const results = [];
  for (const item of cases) {
    try { await item.run(); results.push({name: item.name, passed: true}); }
    catch (error) { results.push({name: item.name, passed: false, error: String(error.stack || error)}); }
    document.querySelector('#results').textContent = JSON.stringify(results, null, 2);
  }
  document.querySelector('#results').textContent = JSON.stringify(results, null, 2);
  document.body.dataset.failed = String(results.filter(r => !r.passed).length);
  document.body.dataset.total = String(results.length);
  document.body.dataset.done = 'true';
}
