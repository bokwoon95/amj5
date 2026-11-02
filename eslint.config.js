const jsdoc = require("eslint-plugin-jsdoc");

module.exports = [
  {
    files: ["src/javascript/**/*.js"],
    plugins: {
      jsdoc,
    },
    rules: {
      curly: ["error", "all"],
      "jsdoc/multiline-blocks": [
        "error",
        {
          noSingleLineBlocks: true,
          singleLineTags: [],
        },
      ],
    },
  },
];
