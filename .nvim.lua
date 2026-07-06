-- Register filetypes: .zup is plain source, .zupt is a sectioned test file
vim.filetype.add({
  extension = {
    zup = "zup",
  },
})

-- Plain .zup source: highlight the whole buffer with window-local matches.
vim.api.nvim_create_autocmd("FileType", {
  pattern = "zup",
  callback = function()
    for _, match in ipairs(vim.fn.getmatches()) do
      if match.group:find("^zup") or match.group == "Keyword" then
        vim.fn.matchdelete(match.id)
      end
    end

    -- Keywords
    vim.fn.matchadd("Keyword",
      [[\v<(pub|fn|return|as|if|else|const|let|while|for|break|continue|in|extern|struct|enum|match|defer)>]])
    -- Types
    vim.fn.matchadd("Type", [[\v<(i8|u8|i16|u16|i32|u32|i64|u64|f32|f64|void|bool|cstr|str|string)>]])
    -- Slice/array type prefixes: []T and [N]T (highlight the brackets when before a type)
    vim.fn.matchadd("Type", [[\v\[\d*\](\a)@=]])
    -- Boolean literals; link to Number so themes that paint Boolean like
    -- Keyword still render them as values, not control-flow words
    vim.api.nvim_set_hl(0, "Boolean", { link = "Number" })
    vim.fn.matchadd("Boolean", [[\v<(true|false)>]])
    -- Numbers
    vim.fn.matchadd("Number", [[\v<\d+>]])
    -- Comments (line and block, block may span multiple lines)
    vim.fn.matchadd("Comment", [[\v\/\/.*$]])
    vim.fn.matchadd("Comment", [[\v\/\*\_.{-}\*\/]])
    -- Char literals: 'x' and escapes like '\n', '\''
    vim.fn.matchadd("String", [[\v'%(\\.|[^'\\])']])
    -- Strings (may span multiple lines; \_[^"] matches any char including newline except a closing quote).
    -- '@1<! keeps the " inside the char literal '"' from opening a string that swallows the rest of the file.
    vim.fn.matchadd("String", [[\v'@1<!"%(\\.|\_[^"])*"]])
  end,
})
