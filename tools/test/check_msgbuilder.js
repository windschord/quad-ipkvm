#!/usr/bin/env node
// Checks the WS /ws message builder in components/web_server/web/app.js byte by byte against docs/WEB_API.md 6
// (little-endian numbers; type 0x01 KEY_DOWN, 0x02 KEY_UP, 0x03 RELEASE_ALL, 0x04 KEEPALIVE, 0x10 PTR_ABS, 0x11 PTR_REL).
// The expected bytes are written out by hand, not computed. `--mutations` runs against mutated copies of app.js.
// Usage: node tools/test/check_msgbuilder.js [--mutations]      (no dependencies)
'use strict';
const fs = require('fs');
const path = require('path');

const APP_JS = path.join(__dirname, '..', '..', 'components', 'web_server', 'web', 'app.js');

const CASES = [
    ['keyDown(0x04)', (b) => b.keyDown(0x04), [0x01, 0x04]],
    ['keyDown(0xE7)', (b) => b.keyDown(0xE7), [0x01, 0xE7]],
    ['keyUp(0x2C)', (b) => b.keyUp(0x2C), [0x02, 0x2C]],
    ['releaseAll()', (b) => b.releaseAll(), [0x03]],
    ['keepalive()', (b) => b.keepalive(), [0x04]],
    ['ptrAbs(5, 0x1234, 0x5678, -1)', (b) => b.ptrAbs(5, 0x1234, 0x5678, -1), [0x10, 0x05, 0x34, 0x12, 0x78, 0x56, 0xFF]],
    ['ptrAbs(0, 0, 32767, 127)', (b) => b.ptrAbs(0, 0, 32767, 127), [0x10, 0x00, 0x00, 0x00, 0xFF, 0x7F, 0x7F]],
    ['ptrRel(3, -2, 300, 127)', (b) => b.ptrRel(3, -2, 300, 127), [0x11, 0x03, 0xFE, 0xFF, 0x2C, 0x01, 0x7F]],
    ['ptrRel(0, -32768, 32767, -127)', (b) => b.ptrRel(0, -32768, 32767, -127), [0x11, 0x00, 0x00, 0x80, 0xFF, 0x7F, 0x81]],
];

function loadBuilder(source) {
    const m = /const MsgBuilder = (\{[\s\S]*?\n    \});/.exec(source);
    if (!m) throw new Error('MsgBuilder not found in app.js');
    return new Function('return ' + m[1])();
}

function check(source) {
    const problems = [];
    let builder;
    try {
        builder = loadBuilder(source);
    } catch (e) {
        return [String(e.message)];
    }
    for (const [name, call, expected] of CASES) {
        let got;
        try {
            got = Array.from(new Uint8Array(call(builder)));
        } catch (e) {
            problems.push(name + ': threw ' + e.message);
            continue;
        }
        if (got.length !== expected.length || got.some((v, i) => v !== expected[i])) {
            problems.push(name + ': expected ' + JSON.stringify(expected) + ', got ' + JSON.stringify(got));
        }
    }
    return problems;
}

// Not listed: setInt16 -> setUint16 for dx/dy (equivalent: DataView wraps negative values modulo 2^16, same bytes).
function mutants(source) {
    const list = [
        ['KEY_DOWN type 0x01 -> 0x02', "v.setUint8(0, 0x01);", "v.setUint8(0, 0x02);"],
        ['KEY_UP type 0x02 -> 0x01', "v.setUint8(0, 0x02);", "v.setUint8(0, 0x01);"],
        ['RELEASE_ALL type 0x03 -> 0x04', "v.setUint8(0, 0x03);", "v.setUint8(0, 0x04);"],
        ['KEEPALIVE type 0x04 -> 0x03', "v.setUint8(0, 0x04);", "v.setUint8(0, 0x03);"],
        ['PTR_ABS type 0x10 -> 0x11', "v.setUint8(0, 0x10);", "v.setUint8(0, 0x11);"],
        ['PTR_REL type 0x11 -> 0x10', "v.setUint8(0, 0x11);", "v.setUint8(0, 0x10);"],
        ['PTR_ABS x big-endian', "v.setUint16(2, x, true);", "v.setUint16(2, x, false);"],
        ['PTR_ABS y big-endian', "v.setUint16(4, y, true);", "v.setUint16(4, y, false);"],
        ['PTR_REL dx big-endian', "v.setInt16(2, dx, true);", "v.setInt16(2, dx, false);"],
        ['PTR_REL dy big-endian', "v.setInt16(4, dy, true);", "v.setInt16(4, dy, false);"],
        ['PTR_ABS wheel dropped', "v.setInt8(6, wheel);\n            return buf;\n        },\n        ptrRel", "return buf;\n        },\n        ptrRel"],
        ['PTR_ABS buttons dropped', "v.setUint8(1, buttons);\n            v.setUint16(2, x, true);", "v.setUint16(2, x, true);"],
        ['PTR_REL buttons dropped', "v.setUint8(1, buttons);\n            v.setInt16(2, dx, true);", "v.setInt16(2, dx, true);"],
        ['KEY_DOWN usage dropped', "v.setUint8(1, usage);\n            return buf;\n        },\n        keyUp", "return buf;\n        },\n        keyUp"],
        ['KEY_UP length 2 -> 3', "keyUp(usage) {\n            const buf = new ArrayBuffer(2);", "keyUp(usage) {\n            const buf = new ArrayBuffer(3);"],
        ['PTR_ABS length 7 -> 8', "ptrAbs(buttons, x, y, wheel) {\n            const buf = new ArrayBuffer(7);", "ptrAbs(buttons, x, y, wheel) {\n            const buf = new ArrayBuffer(8);"],
    ];
    return list.map(([name, from, to]) => [name, source.includes(from) ? source.replace(from, to) : source]);
}

const source = fs.readFileSync(APP_JS, 'utf8');
let status = 0;
const problems = check(source);
problems.forEach((p) => console.log('FAIL:', p));
if (problems.length) status = 1; else console.log('msgbuilder ok (' + CASES.length + ' cases)');

if (process.argv.includes('--mutations')) {
    let survived = 0;
    const ms = mutants(source);
    for (const [name, mutated] of ms) {
        const unchanged = mutated === source;
        const detected = !unchanged && check(mutated).length > 0;
        console.log((unchanged ? 'NOMATCH ' : detected ? 'KILLED  ' : 'SURVIVED') + ' ' + name);
        if (!detected) survived++;
    }
    console.log(ms.length + ' mutants, ' + survived + ' survived');
    if (survived) status = 1;
}
process.exit(status);
