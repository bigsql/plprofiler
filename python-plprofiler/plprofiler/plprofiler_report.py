#!/usr/bin/env python

import base64
import cgi
import os
import subprocess
import sys

__all__ = ['plprofiler_report']

class plprofiler_report:
    def __init__(self):
        pass

    def generate(self, report_data, outfd):
        config = report_data['config']
        self.outfd = outfd

        self.out("<html>")
        self.out("<head>")
        self.out("  <title>%s</title>" %(cgi.escape(config['title']), ))
        self.out(HTML_SCRIPT)
        self.out(HTML_STYLE)
        self.out("</head>")
        self.out("""<body bgcolor="#ffffff" onload="set_stat_bars()">""")
        self.out(config['desc'])

        self.out("<h2>PL/pgSQL Call Graph</h2>")
        self.out("<center>")
        self.out(self.generate_flamegraph(config, report_data['flamedata']))
        self.out("</center>")

        if not report_data['func_oids_by_user']:
            if report_data['found_more_funcs']:
                hdr = "<h2>Top %d functions (by self_time)</h2>" %(len(report_data['func_list']),)
            else:
                hdr = "<h2>All %d functions (by self_time)</h2>" %(len(report_data['func_list']),)
        else:
            hdr = "<h2>Requested functions</h2>"

        self.out("<h2>List of functions detailed below</h2>")
        self.out("<ul>")
        for func in report_data['func_list']:
            self.out("""<li><a href="#A{funcoid}">{schema}.{funcname}() oid={funcoid}</a></li>""".format(**func))
        self.out("</ul>")
        self.out(hdr)

        for func_def in report_data['func_defs']:
            self.generate_function_output(config, func_def)

        self.out("</body>")
        self.out("</html>")

    def format_d_comma(self, num):
        s = str(num)
        r = []
        l = len(s)
        i = 0
        j = l % 3
        if j == 0:
            j = 3
        while j <= l:
            r.append(s[i:j])
            i = j
            j += 3
        return ",".join(r)

    def generate_function_output(self, config, func_def):
        func_def['self_time_fmt'] = self.format_d_comma(func_def['self_time'])
        func_def['total_time_fmt'] = self.format_d_comma(func_def['total_time'])
        self.out("""<a name="A{funcoid}" />""".format(**func_def))
        self.out("""<h3>Function {schema}.{funcname}() oid={funcoid} (<a id="toggle_{funcoid}"
                href="javascript:toggle_div('toggle_{funcoid}', 'div_{funcoid}')">show</a>)</h3>""".format(**func_def))
        self.out("""<p>""")
        self.out("""self_time = {self_time_fmt:s} &micro;s<br/>""".format(**func_def))
        self.out("""total_time = {total_time_fmt:s} &micro;s""".format(**func_def))
        self.out("""</p>""")
        self.out("""<table border="0" cellpadding="0" cellspacing="0">""")
        self.out("""  <tr>""")
        self.out("""    <td valign="top"><b><code>{schema}.{funcname}&nbsp;</code></b></td>""".format(**func_def))
        self.out("""    <td><b><code>({funcargs})</code></b></td>""".format(
                     funcargs = func_def['funcargs'].replace(', ', ',<br/>&nbsp;')))
        self.out("""  </tr>""")
        self.out("""  <tr>""")
        self.out("""    <td colspan="2">""")
        self.out("""      <b><code>&nbsp;&nbsp;&nbsp;&nbsp;RETURNS&nbsp;{funcresult}</code></b>""".format(
                    funcresult = func_def['funcresult'].replace(' ', '&nbsp;')))
        self.out("""    </td>""")
        self.out("""  </tr>""")
        self.out("""</table>""")

        self.out("""<div id="div_{funcoid}" style="display: none">""".format(
                **func_def))

        self.out("<center>")
        self.out("""<table class="linestats" border="1" cellpadding="0" cellspacing="0" width="%s">""" %(config['table_width'], ))
        self.out("""  <tr>""")
        self.out("""    <th width="10%">Line</th>""")
        self.out("""    <th width="10%">exec_count</th>""")
        self.out("""    <th width="10%">total_time</th>""")
        self.out("""    <th width="10%">longest_time</th>""")
        self.out("""    <th width="60%">Source Code</th>""")
        self.out("""  </tr>""")

        for line in func_def['source']:
            if line['line_number'] == 0:
                src = "<b>--&nbsp;Function&nbsp;Totals</b>"
            else:
                src = cgi.escape(line['source'].expandtabs(int(config['tabstop']))).replace(" ", "&nbsp;")
            self.out("""  <tr>""")
            self.out("""    <td align="right"><code>{val}</code></td>""".format(val = line['line_number']))
            self.out("""    <td align="right">{val}</td>""".format(val = line['exec_count']))
            self.out("""    <td class="bar" align="right">{val}</td>""".format(val = line['total_time']))
            self.out("""    <td align="right">{val}</td>""".format(val = line['longest_time']))
            self.out("""    <td align="left"><code>{src}</code></td>""".format(src = src))
            self.out("""  </tr>""")

        self.out("</table>")
        self.out("</center>")
        self.out("</div>")

    def generate_flamegraph(self, config, data):
        path = os.path.dirname(os.path.abspath(__file__))
        path = os.path.join(path, 'lib', 'FlameGraph', 'flamegraph.pl', )

        proc = subprocess.Popen(['perl', path,
                    "--title=%s" %(config['title'], ),
                    "--width=%s" %(config['svg_width'], ), ],
                    stdin = subprocess.PIPE,
                    stdout = subprocess.PIPE,
                    stderr = subprocess.PIPE);
        svg, err = proc.communicate(data)

        if proc.returncode != 0:
            raise Exception("flamegraph returned with exit code %d\n%s" %(
                    proc.returncode, err))
        return svg
        # return "\n".join(svg.split("\n")[2:])

    def out(self, line):
        self.outfd.write(line + '\n')

HTML_SCRIPT = """
  <script language="javascript">
    // ----
    // toggle_div()
    //
    //  JS function to toggle one of the functions to show/block.
    // ----
    function toggle_div(hs_id, div_id) {
        var h_elem = document.getElementById(hs_id);
        var d_elem = document.getElementById(div_id);
        if (d_elem.style.display == "block") {
            d_elem.style.display = "none";
            h_elem.innerHTML = "show";
        } else {
            d_elem.style.display = "block";
            h_elem.innerHTML = "hide";
        }
    }

    // ----
    // set_stat_bars()
    // ----
    function set_stat_bars() {
        // Loop over all tables that are of linestats class.
        var tbls = document.getElementsByClassName("linestats");
        for (var i = 0; i < tbls.length; i++) {
            tbl = tbls[i];
            rws = tbl.rows;

            // Get the function level totals of the counters from the
            // second row (the pseudo line-0 row).
            vals = rws[1].getElementsByTagName("td");
            var exec_max = parseFloat(vals[1].innerHTML)
            var total_max = parseFloat(vals[2].innerHTML)
            var longest_max = parseFloat(vals[3].innerHTML)

            // Guard against division by zero errors.
            if (exec_max == 0) exec_max = 1;
            if (total_max == 0) total_max = 1;
            if (longest_max == 0) longest_max = 1;

            // Now we can calculate and set the bars relative to those
            // function level totals.
            for (var j = 1; j < rws.length; j++) {
                var vals = rws[j].getElementsByTagName("td");

                var val = parseFloat(vals[1].innerHTML)
                // var pct = val / exec_max * 100;
                // vals[1].style.backgroundSize = pct + "% 100%";
                // vals[1].style.backgroundSize = "0% 100%";
                vals[1].innerHTML = "<code>" + val.toLocaleString() + "</code>";

                val = parseFloat(vals[2].innerHTML)
                pct = val / total_max * 100;
                pct_str = "(" + pct.toFixed(2) + "%)"
                var need_spc = 10 - pct_str.length;
                for (var k = 0; k < need_spc; k++) {
                    pct_str = "&nbsp;" + pct_str;
                }
                vals[2].style.backgroundSize = pct + "% 100%";
                vals[2].innerHTML = "<code>" + val.toLocaleString() + "&nbsp;&micro;s" + pct_str + "</code>";

                val = parseFloat(vals[3].innerHTML)
                // pct = val / longest_max * 100;
                // vals[3].style.backgroundSize = pct + "% 100%";
                vals[3].innerHTML = "<code>" + val.toLocaleString() + "&nbsp;&micro;s" + "</code>";
            }
        }
    }
  </script>
"""

HTML_STYLE = """
  <style>
    h1,h2,h3,h4	{ color:#2222AA;
                }

    h1		{ font-family: Helvetica,Arial;
                  font-weight: 700;
                  font-size: 24pt;
                }

    h2		{ font-family: Helvetica,Arial;
                  font-weight: 700;
                  font-size: 18pt;
                }

    h3,h4	{ font-family: Helvetica,Arial;
                  font-weight: 700;
                  font-size: 16pt;
                }

    p,li,dt,dd	{ font-family: Helvetica,Arial;
                  font-size: 14pt;
                }

    pre		{ font-family: Courier,Fixed;
                  font-size: 14pt;
                }

    samp	{ font-family: Courier,Fixed;
                  font-weight: 900;
                  font-size: 14pt;
                }

    big		{ font-weight: 900;
                  font-size: 120%;
                }

    .bar        { background-color: white;
                  background-image: url(data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAgAAAAICAIAAABLbSncAAAACXBIWXMAAAsTAAALEwEAmpwYAAAAB3RJTUUH4AYEDho2r7uRnQAAAB1pVFh0Q29tbWVudAAAAAAAQ3JlYXRlZCB3aXRoIEdJTVBkLmUHAAAAFUlEQVQI12P8dlOcARtgYsABBqcEAH/aAfYx26o/AAAAAElFTkSuQmCC);
                  background-repeat: no-repeat;
                }
  </style>
"""

