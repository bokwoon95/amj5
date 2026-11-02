const jsdoc = require("eslint-plugin-jsdoc");
const prettierRecommended = require("eslint-plugin-prettier/recommended");

module.exports = [
  prettierRecommended,
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
