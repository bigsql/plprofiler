#!/usr/bin/env python

import re
import sys

def main():
    for fname in sys.argv[1:]:
        with open(fname, 'r') as fd:
            statements = sql_split(fd.read()).statements
        # print statements

class sql_split:
    def __init__(self, sql_str):
        self.sql_str = sql_str
        self.sql_len = len(sql_str)
        self.sql_idx = 0

        self.statements = []
        self.cur_stmt = ""
        self.paren_level = 0
        self.dol_quote_re = re.compile("(\\$[^$\\s]*\\$)")

        self.state_normal()

        if not self.cur_stmt.isspace():
            self.have_stmt()

    def get_statements(self):
        return self.statements

    def state_normal(self):
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx] == "'":
                self.state_quote()
            elif self.sql_str[self.sql_idx] == '"':
                self.state_d_quote()
            elif self.sql_str[self.sql_idx:self.sql_idx + 2] == "--":
                self.state_sql_comment()
            elif self.sql_str[self.sql_idx:self.sql_idx + 2] == "/*":
                self.state_C_comment()
            elif self.sql_str[self.sql_idx:self.sql_idx + 2] == "E'":
                self.state_e_quote("E'")
            elif self.sql_str[self.sql_idx:self.sql_idx + 2] == "e'":
                self.state_e_quote("e'")
            elif self.sql_str[self.sql_idx] == "(":
                self.cur_stmt += "("
                self.sql_idx += 1
                self.paren_level += 1
            elif self.sql_str[self.sql_idx] == ")":
                self.cur_stmt += ")"
                self.sql_idx += 1
                self.paren_level -= 1
            elif self.sql_str[self.sql_idx] == "[":
                self.cur_stmt += "["
                self.sql_idx += 1
                self.paren_level += 1
            elif self.sql_str[self.sql_idx] == "]":
                self.cur_stmt += "]"
                self.sql_idx += 1
                self.paren_level -= 1
            elif self.sql_str[self.sql_idx] == ";":
                self.cur_stmt += ";"
                self.sql_idx += 1
                if self.paren_level == 0:
                    self.have_stmt()
            else:
                m = self.dol_quote_re.match(self.sql_str[self.sql_idx:])
                if m is not None:
                    self.state_dollar_quote(m.groups()[0])
                else:
                    self.cur_stmt += self.sql_str[self.sql_idx]
                    self.sql_idx += 1

    def have_stmt(self):
        if self.cur_stmt.strip() != "":
            self.statements.append(self.cur_stmt)
        self.cur_stmt = ""
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx].isspace():
                self.sql_idx += 1
            else:
                break

    def state_sql_comment(self):
        self.cur_stmt += "--"
        self.sql_idx += 2
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx:self.sql_idx + 2] == '\r\n':
                self.cur_stmt += '\r\n'
                self.sql_idx += 2
                return
            elif self.sql_str[self.sql_idx] == '\n':
                self.cur_stmt += '\n'
                self.sql_idx += 1
                return
            else:
                self.cur_stmt += self.sql_str[self.sql_idx]
                self.sql_idx += 1

    def state_C_comment(self):
        self.cur_stmt += "/*"
        self.sql_idx += 2
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx:self.sql_idx + 2] == '*/':
                self.cur_stmt += '*/'
                self.sql_idx += 2
                return
            else:
                self.cur_stmt += self.sql_str[self.sql_idx]
                self.sql_idx += 1

    def state_quote(self):
        self.cur_stmt += "'"
        self.sql_idx += 1
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx:self.sql_idx + 2] == "''":
                self.cur_stmt += "''"
                self.sql_idx += 2
            elif self.sql_str[self.sql_idx] == "'":
                self.cur_stmt += "'"
                self.sql_idx += 1
                return
            else:
                self.cur_stmt += self.sql_str[self.sql_idx]
                self.sql_idx += 1

    def state_d_quote(self):
        self.cur_stmt += '"'
        self.sql_idx += 1
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx:self.sql_idx + 2] == '""':
                self.cur_stmt += '""'
                self.sql_idx += 2
            elif self.sql_str[self.sql_idx] == '"':
                self.cur_stmt += '"'
                self.sql_idx += 1
                return
            else:
                self.cur_stmt += self.sql_str[self.sql_idx]
                self.sql_idx += 1

    def state_e_quote(self):
        self.cur_stmt += "'"
        self.sql_idx += 1
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx] == '\\':
                self.cur_stmt += self.sql_str[self.sql_idx:self.sql_idx + 2]
                self.sql_idx += 2
            elif self.sql_str[self.sql_idx:self.sql_idx + 2] == "''":
                self.cur_stmt += "''"
                self.sql_idx += 2
            elif self.sql_str[self.sql_idx] == "'":
                self.cur_stmt += "'"
                self.sql_idx += 1
                return
            else:
                self.cur_stmt += self.sql_str[self.sql_idx]
                self.sql_idx += 1

    def state_dollar_quote(self, tag):
        tag_len = len(tag)
        self.cur_stmt += tag
        self.sql_idx += tag_len
        while self.sql_idx < self.sql_len:
            if self.sql_str[self.sql_idx:self.sql_idx + tag_len] == tag:
                self.cur_stmt += tag
                self.sql_idx += tag_len
                return
            self.cur_stmt += self.sql_str[self.sql_idx]
            self.sql_idx += 1

if __name__ == '__main__':
    main()
