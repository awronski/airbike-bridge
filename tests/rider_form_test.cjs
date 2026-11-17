// Run the actual form handler from both pages, using an in-memory API and DOM.
const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const vm = require('node:vm');
const {test} = require('node:test');

for (const page of ['index.html', 'hr-mock.html']) {
  test(`${page}: create, correct and save keeps one profile`, async () => {
    const html = readFileSync(join(__dirname, '../web', page), 'utf8');
    const functions = html.slice(html.indexOf('function fillRiders('), html.indexOf('async function loadSettings('));
    const handler = html.slice(html.indexOf("el('rider-form').addEventListener"), html.indexOf("el('auto-form').addEventListener"));
    const controls = new Map();
    let submit;
    const el = id => {
      if (!controls.has(id)) controls.set(id, {
        value: ['rider-select', 'edit-rider', 'rider-sex'].includes(id) ? '0' : '',
        replaceChildren(...options) { this.options = options; this.value = options[0]?.value || ''; },
        addEventListener(event, fn) { if (id === 'rider-form' && event === 'submit') submit = fn; }
      });
      return controls.get(id);
    };
    const people = [];
    const calls = [];
    const context = vm.createContext({
      el, document: {createElement: () => ({value: '', textContent: ''})},
      riders: [], result() {}, updateBandLink() {},
      async post(url, action, fields) {
        assert.equal(url, '/api/riders'); assert.equal(action, 'riders');
        calls.push({...fields});
        const id = Number(fields.id) || people.length + 1;
        const rider = {...fields, id, band_address: ''};
        const index = people.findIndex(r => r.id === id);
        if (index < 0) people.push(rider); else people[index] = rider;
        return {saved_id: id, riders: structuredClone(people), mode: 'manual', selected_id: 0};
      }
    });
    vm.runInContext(functions + handler, context);
    el('rider-name').value = 'New rider';
    const button = {disabled: false};
    await submit({preventDefault() {}, submitter: button});
    assert.equal(el('edit-rider').value, '1', 'successful create must enter edit mode');
    assert.equal(el('rider-select').value, '0', 'editing does not switch the active rider');
    el('rider-name').value = 'Corrected name';
    await submit({preventDefault() {}, submitter: button});
    assert.equal(calls[1].id, '1');
    assert.equal(people.length, 1);
    assert.equal(people[0].name, 'Corrected name');
    assert.equal(button.disabled, false);
  });
}
