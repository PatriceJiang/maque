const fs = require('fs')
const path = require('path');
const YAML = require('yaml');

const schemaPath = path.join(__dirname, 'schema.yaml');

const data = YAML.parse(fs.readFileSync(schemaPath, 'utf8'));

const $authors = [
    '',
    '/**',
    ' * @author Gen.PT',
    ' */'
];
const $preincludes = [
    '#include <cstdint>',
];

const $headers = ["#pragma once"]


class Formatter {
    constructor() {
        this.indent = 0;
        this.rows = [];
        let ws = "               ";
        ws += ws;
        ws += ws;
        ws += ws;
        ws += ws;
        ws += ws;
        ws += ws;
        ws += ws;
        this.whitespace = ws;
    }

    enterScope() {
        this.indent += 4;
    }

    exitScope() {
        this.indent -= 4;
    }


    push(line) {
        this.rows.push(this.whitespace.slice(0, this.indent) + line);
    }

    newLine() {
        this.rows.push("");
    }
}

function FieldDeclare(type, name) {
    const arr = type.match(/\[\d+\]$/)
    if(arr) {
        return `${type.slice(0, arr.index)} ${name}${arr[0]}`
    }
    const dynarr = type.match(/\[\]$/)
    if(dynarr) {
        return `std::vector<${type.slice(0, dynarr.index)}> ${name}`
    } 
    return `${type} ${name}`
}

function GenerateClass(namespace, classInfo) {
    let includes = [];
    let classLines = new Formatter();
    classLines.newLine();
    classLines.push(`namespace ${namespace} {`);
    classLines.newLine();
    classLines.push(`class ${classInfo.class} {`);
    classLines.push(`public:`);
    classLines.enterScope();
    if(classInfo.size) {
        classLines.push(`static_assert(sizeof(${classInfo.class}) == ${classInfo.size});`);
    }
    for (let field of classInfo.fields) {
        if (field.default) {
            classLines.push(`${FieldDeclare(field.type, field.name)} = ${field.default};`);
        } else {
            classLines.push(`${FieldDeclare(field.type, field.name)};`);
        }
    }
    classLines.exitScope();
    classLines.push(`}; // end class ${classInfo.class}`);
    classLines.newLine();
    classLines.push(`} // end namespace ${namespace}`);
    classLines.newLine();

    return $headers.concat($authors).concat($preincludes).concat(includes).concat(classLines.rows).join('\n');
}

for (let namespace in data) {
    for (let classInfo of data[namespace].class_list) {
        if (classInfo.class) {
            console.log(GenerateClass(data[namespace].namespace, classInfo));
        }
    }
}