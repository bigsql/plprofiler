#!/usr/bin/env python

import sys
import subprocess
import cgi
import psycopg2
import getopt
import base64
import json

import plprofiler_data

__all__ = ['report']

def report(argv):
    opt_conninfo = ''
    opt_name = None
    opt_top = 10
    opt_output = None

    global output_fd

    try:
        opts, args = getopt.getopt(argv, "c:n:o:t:", [
                'conninfo=', 'name=', 'output=', 'top=', ])
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 2

    for opt, val in opts:
        if opt in ('-c', '--conninfo', ):
            opt_conninfo = val
        elif opt in ('-n', '--name', ):
            opt_name = val
        elif opt in ('-o', '--output', ):
            opt_output = val
        elif opt in ('-t', '--top', ):
            opt_top = int(val)

    if opt_name is None:
        sys.write.stderr("option --name must be given\n")
        return 2

    if opt_output is None:
        output_fd = sys.stdout
    else:
        output_fd = open(opt_output, 'w')

    db = psycopg2.connect(opt_conninfo)
    cur = db.cursor()
    try:
        profiler_namespace = plprofiler_data.get_profiler_namespace(db);
    except Exception as err:
        sys.stderr.write(str(err) + '\n')
        return 1

    cur.execute("""SET search_path TO %s""", (profiler_namespace, ))
    cur.execute("""SELECT s_options
                    FROM pl_profiler_saved
                    WHERE s_name = %s""", (opt_name, ))
    row = cur.fetchone()
    if row is None:
        print "No saved data with name '" + opt_name + "' found"
        db.rollback()
        return 1
    config = json.loads(row[0])

    out("<html>")
    out("<head>")
    out("  <title>%s</title>" %(cgi.escape(config['title']), ))
    out(HTML_SCRIPT)
    out(HTML_STYLE)
    out("</head>")
    out("""<body bgcolor="#ffffff" onload="set_stat_bars()">""")
    out(config['desc'])

    out("<h2>PL/pgSQL Call Graph</h2>")
    out("<center>")
    out(generate_flamegraph(db, opt_name, config))
    out("</center>")

    if len(args) == 0:
        out("<h2>Top %d functions (by total_time)</h2>" %(opt_top))
        cur.execute("""SELECT l_funcoid,
                              coalesce(l_total_time, 0) as total_time
                        FROM pl_profiler_saved S
                        LEFT JOIN pl_profiler_saved_linestats L
                            ON L.l_s_id = S.s_id
                        WHERE S.s_name = %s AND L.l_line_number = 0
                        ORDER BY total_time DESC
                        LIMIT %s""", (opt_name, opt_top, ))
        func_oids = []
        for row in cur:
            func_oids.append(int(row[0]))
    else:
        out("<h2>Requested functions</h2>")
        func_oids = [int(x) for x in args]

    for func_oid in func_oids:
        generate_function_output(db, opt_name, config, func_oid)

    out("</body>")
    out("</html>")

    if opt_output is not None:
        output_fd.close()

    db.close()
    return 0

def generate_function_output(db, opt_name, config, func_oid):
    cur = db.cursor()
    cur.execute("""SELECT l_funcoid, f_funcname, f_funcresult, f_funcargs,
                    coalesce(l_total_time, 0) as total_time
                    FROM pl_profiler_saved S
                    LEFT JOIN pl_profiler_saved_linestats L ON l_s_id = s_id
                    JOIN pl_profiler_saved_functions F ON f_funcoid = l_funcoid
                    WHERE S.s_name = %s
                      AND L.l_funcoid = %s
                      AND L.l_line_number = 0""",
                (opt_name, func_oid, ))
    row = cur.fetchone()
    if row is None:
        sys.stderr.write("function with Oid %d not found\n" %func_oid)
        return

    out("""<h3>Function {func_name}() oid={oid} (<a id="toggle_{oid}"
            href="javascript:toggle_div('toggle_{oid}', 'div_{oid}')">show</a>)</h3>""".format(oid = func_oid,
               func_name = row[1]))
    out("""<p>total_time = {time:,d} &micro;s</p>""".format(time = int(row[4])))
    out("""<table border="0" cellpadding="0" cellspacing="0">""")
    out("""  <tr>""")
    out("""    <td valign="top"><b><code>{func_name}&nbsp;</code></b></td>""".format(
                 func_name = row[1]))
    out("""    <td><b><code>({func_args})</code></b></td>""".format(
                 func_args = row[3].replace(', ', ',<br/>&nbsp;')))
    out("""  </tr>""")
    out("""  <tr>""")
    out("""    <td colspan="2">""")
    out("""      <b><code>&nbsp;&nbsp;&nbsp;&nbsp;RETURNS&nbsp;{func_result}</code></b>""".format(
                func_result = row[2].replace(' ', '&nbsp;')))
    out("""    </td>""")
    out("""  </tr>""")
    out("""</table>""")
                
    out("""<div id="div_{oid}" style="display: none">""".format(
            oid = func_oid))

    out("<center>")
    out("""<table class="linestats" border="1" cellpadding="0" cellspacing="0" width="%s">""" %(config['table_width'], ))
    out("""  <tr>""")
    out("""    <th width="10%">Line</th>""")
    out("""    <th width="60%">Source Code</th>""")
    out("""    <th width="10%">exec_count</th>""")
    out("""    <th width="10%">total_time</th>""")
    out("""    <th width="10%">longest_time</th>""")
    out("""  </tr>""")

    cur.execute("""SELECT l_line_number, l_source, l_exec_count,
                    l_total_time, l_longest_time
                    FROM pl_profiler_saved S
                    JOIN pl_profiler_saved_linestats L ON L.l_s_id = S.s_id
                    WHERE S.s_name = %s
                      AND L.l_funcoid = %s
                    ORDER BY l_s_id, l_funcoid, l_line_number""",
                    (opt_name, func_oid, ))
    for row in cur:
        if row[0] == 0:
            src = "<b>--&nbsp;Function&nbsp;Totals</b>"
        else:
            src = cgi.escape(row[1].expandtabs(int(config['tabstop']))).replace(" ", "&nbsp;")
        out("""  <tr>""")
        out("""    <td align="right"><code>{val}</code></td>""".format(val = row[0]))
        out("""    <td align="left"><code>{src}</code></td>""".format(src = src))
        out("""    <td align="right">{val}</td>""".format(val = row[2]))
        out("""    <td class="bar" align="right">{val}</td>""".format(val = row[3]))
        out("""    <td align="right">{val}</td>""".format(val = row[4]))
        out("""  </tr>""")

    out("</table>")
    out("</center>")
    out("</div>")

    cur.close()

def generate_flamegraph(db, opt_name, config):
    cur = db.cursor()
    cur.execute("""SELECT array_to_string(c_stack, ';'), c_us_self
                    FROM pl_profiler_saved S
                    JOIN pl_profiler_saved_callgraph C ON C.c_s_id = S.s_id
                    WHERE S.s_name = %s""",
                (opt_name, ))
    data = ""
    for row in cur:
        data += str(row[0]) + " " + str(row[1]) + "\n"
    cur.close()
    
    proc = subprocess.Popen(["/home/wieck/FlameGraph/flamegraph.pl",
                "--title=%s" %(config['title'], ),
                "--width=%s" %(config['svg_width'], ), ],
                stdin = subprocess.PIPE,
                stdout = subprocess.PIPE,
                stderr = subprocess.PIPE);
    svg, err = proc.communicate(data)

    if proc.returncode != 0:
        raise Exception("flamegraph returned with exit code %d\n%s" %(
                proc.returncode, err))
    return "\n".join(svg.split("\n")[2:])

def out(line):
    global output_fd
    output_fd.write(line + '\n')

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
            var exec_max = parseFloat(vals[2].innerHTML)
            var total_max = parseFloat(vals[3].innerHTML)
            var longest_max = parseFloat(vals[4].innerHTML)

            // Guard against division by zero errors.
            if (exec_max == 0) exec_max = 1;
            if (total_max == 0) total_max = 1;
            if (longest_max == 0) longest_max = 1;

            // Now we can calculate and set the bars relative to those
            // function level totals.
            for (var j = 1; j < rws.length; j++) {
                var vals = rws[j].getElementsByTagName("td");

                var val = parseFloat(vals[2].innerHTML)
                // var pct = val / exec_max * 100;
                // vals[2].style.backgroundSize = pct + "% 100%";
                // vals[2].style.backgroundSize = "0% 100%";
                vals[2].innerHTML = "<code>" + val.toLocaleString() + "</code>";

                val = parseFloat(vals[3].innerHTML)
                pct = val / total_max * 100;
                pct_str = "(" + pct.toFixed(2) + "%)"
                var need_spc = 10 - pct_str.length;
                for (var k = 0; k < need_spc; k++) {
                    pct_str = "&nbsp;" + pct_str;
                }
                vals[3].style.backgroundSize = pct + "% 100%";
                vals[3].innerHTML = "<code>" + val.toLocaleString() + "&nbsp;&micro;s" + pct_str + "</code>";

                val = parseFloat(vals[4].innerHTML)
                // pct = val / longest_max * 100;
                // vals[4].style.backgroundSize = pct + "% 100%";
                vals[4].innerHTML = "<code>" + val.toLocaleString() + "&nbsp;&micro;s" + "</code>";
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

