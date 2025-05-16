const fs = require('fs')
const path = require('path');
const YAML = require('yaml');

const schemaPath = path.join(__dirname, 'schema.yaml');

const data = YAML.parse(fs.readFileSync(schemaPath, 'utf8'));

console.log(data);