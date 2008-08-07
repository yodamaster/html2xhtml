/***************************************************************************
 *   Copyright (C) 2008 by Jesus Arias Fisteus                             *
 *   jaf@it.uc3m.es                                                        *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "tree.h"
#include "cgi.h"
#include "params.h"
#include "dtd_util.h"
#include "procesador.h"
#include "mensajes.h"

#ifdef WITH_CGI

int cgi_status = CGI_ST_UNITIALIZED;

char *boundary = NULL;
int boundary_len = 0;

static int process_params_multipart(const char **input, size_t *input_len);
static int process_params_query_string();
static int mult_skip_boundary(const char **input, size_t *input_len);
static int mult_param_name_len(const char *input, size_t avail);
static int mult_skip_double_eol(const char **input, size_t *input_len);
static int mult_param_value_len(const char *input, size_t avail);
static int set_param(const char *name, size_t name_len,
		     const char *value, size_t value_len);
static void cgi_write_header(void);
static void cgi_write_footer();

#ifdef CGI_DEBUG
static void cgi_debug_write_input(void);
static void cgi_debug_write_state(void);
#endif

int cgi_check_request()
{
  char *method;
  char *type;
  char *query_string;
  char *l;
  int length= -1;

  method = getenv("REQUEST_METHOD");
  type = getenv("CONTENT_TYPE");
  query_string = getenv("QUERY_STRING");

  if (type && method && query_string) {
    l = getenv("CONTENT_LENGTH");
    if (l) 
      length = atoi(l);

    if (strcasecmp(method, "POST"))
      cgi_status = CGI_ERR_METHOD;
    else if (length <= 0) {
      cgi_status = CGI_ERR_OTHER;
    } else if (!strncmp(type, "multipart/form-data; boundary=", 30)) {
      boundary = tree_strdup(&type[30]);
      boundary_len = strlen(boundary);
      cgi_status = CGI_ST_MULTIPART;
    } else if (!strncmp(type,"text/html", 9))
      cgi_status = CGI_ST_DIRECT;
    else
      cgi_status = CGI_ERR_OTHER;
  } else {
    cgi_status = CGI_ST_NOCGI;
  }

  return cgi_status;
} 

int cgi_process_parameters(const char **input, size_t *input_len)
{
  int error = CGI_OK;

  if (cgi_status == CGI_ST_DIRECT) {
    param_cgi_html_output = 0;
    process_params_query_string();
  } else if (cgi_status == CGI_ST_MULTIPART) {
    param_cgi_html_output = 1;
    process_params_multipart(input, input_len);
  } else {
    error = CGI_ERR_NOCGI;
  }

  return error;
}

void cgi_write_error_bad_req()
{
  fprintf(stdout, "Content-Type:%s\n", "text/html");
  
  /* invalid request */
  if (cgi_status == CGI_ERR_METHOD) {
     /* method != POST */
    fprintf(stdout, "Status:405 Method not allowed\n\n");
    if (param_cgi_html_output) {
      fprintf(stdout,"<html><head><title>html2xhtml-Error</title></head><body>\
                      <h1>405 Method not allowed</h1></body></html>");
    }
  }
  else {
    fprintf(stdout,"Status:400 Bad request\n\n");
    if (param_cgi_html_output) {
      fprintf(stdout, "<html><head><title>html2xhtml-Error</title></head>\
                       <body><h1>400 Bad Request</h1></body></html>");
    }
  }  
}

void cgi_write_output()
{
  fprintf(stdout, "Content-Type:%s; charset=iso-8859-1\n\n", "text/html");

  if (param_cgi_html_output) 
    cgi_write_header();

  /* write the XHTML output */
  if (writeOutput()) 
    EXIT("Incorrect state in writeOutput()");

  if (param_cgi_html_output)
    cgi_write_footer();
}

void cgi_write_error(char *msg)
{  
  fprintf(stdout, "Content-Type:%s\n","text/html");
  fprintf(stdout, "Status:400 Bad request\n\n");
  fprintf(stdout, "<html><head><title>html2xhtml-Error</title></head><body>");
  fprintf(stdout, "<h1>400 Bad Request</h1>");
  fprintf(stdout, "<p>An error has been detected while parsing the input");
  if (parser_num_linea > 0) 
    fprintf(stdout, " at line %d. Please, ", parser_num_linea);
  else fprintf(stdout, ". Please, ");
  fprintf(stdout, "check that you have uploaded a HTML document.</p>");
  if (msg)
    fprintf(stdout, "<p>Error: %s</p>", msg);

#ifdef CGI_DEBUG
  cgi_debug_write_state();
#endif

  fprintf(stdout, "</body></html>");  
}

static int process_params_multipart(const char **input, size_t *input_len)
{
  int html_found = 0;
  const char *param_name;
  int param_name_len;
  int param_value_len;

  while (!html_found) {
    /* skip the boundary */
    if (!mult_skip_boundary(input, input_len))
      return CGI_ERR_PARAMS;

    /* read the parameter */
    if (strncmp(*input, "Content-Disposition: form-data; name=\"", 38))
      return CGI_ERR_PARAMS;
    *input += 38;
    *input_len -= 38;
    if ((param_name_len = mult_param_name_len(*input, *input_len)) < 0)
      return CGI_ERR_PARAMS;
    param_name = *input;
    if (!mult_skip_double_eol(input, input_len))
      return CGI_ERR_PARAMS;

    /* it can be the html file or other parameter */
    if (param_name_len == 4 && !strncmp("html", param_name, 4)) {
      html_found = 1;
    } else {
      if ((param_value_len = mult_param_value_len(*input, *input_len)) < 0)
	return CGI_ERR_PARAMS;
      set_param(param_name, param_name_len, *input, param_value_len);
      *input += param_value_len + 2;
      *input_len -= param_value_len + 2;
    }
  }

  return CGI_OK;
}

static int process_params_query_string()
{
  return CGI_ERR_OTHER;
}

static int set_param(const char *name, size_t name_len,
		     const char *value, size_t value_len)
{
  int tmpnum;

  if (name_len == 4) {
    /* param "type"/"tipo" */
    if (!strncmp(name, "type", 4) || !strncmp(name, "tipo", 4)) {
      tmpnum = dtd_get_dtd_index_n(value, value_len);
      if (tmpnum >= 0) {
	param_doctype = tmpnum;
	return 1;
      }
    }
  } else if (name_len == 6) {
    /* param "output"/"salida" */
    if ((!strncmp(name, "output", 6) || !strncmp(name, "salida", 6))
	&& value_len == 5 && !strncmp(value, "plain", 5)) {
      param_cgi_html_output = 0;
      return 1;
    }
  } else if (name_len == 9) {
    /* param "tablen" */
    if (!strncmp(name, "tablength", 9)) {
      char num[value_len + 1];
      memcpy(num, value, value_len);
      num[value_len] = 0;
      tmpnum= atoi(value);
      if (tmpnum >= 0 && tmpnum <= 16) {
	param_tab_len= tmpnum;
	return 1;
      }
    }
  } else if (name_len == 10) {
    /* param "linelength" */
    if (!strncmp(name, "linelength", 10)) {
      char num[value_len + 1];
      memcpy(num, value, value_len);
      num[value_len] = 0;
      tmpnum= atoi(value);
      if (tmpnum >= 40) {
	param_chars_per_line= tmpnum;
	return 1;
      }
    }
  }
}

/*
 * Returns 1 if correctly skipped the boundary, 0 otherwise.
 */
static int mult_skip_boundary(const char **input, size_t *input_len)
{
  int adv = 4 + boundary_len; /* '--' + boundary + '\r\n' */

  if (*input_len < adv || (*input)[0] != '-' || (*input)[1] != '-'
      || (*input)[adv - 2] != '\r' || (*input)[adv - 1] != '\n')
    return 0;

  if (!strncmp(boundary, (*input) + 2, boundary_len)) {
    *input_len -= adv;
    *input += adv;
    return 1;
  } else {
    return 0;
  }
}

static int mult_param_name_len(const char *input, size_t avail)
{
  int len;

  for (len = 0; len < avail && input[len] != '\r' && input[len] != '\"'; len++);
  if (len == avail || input[len] != '\"')
    return -1;
  else
    return len;
}

static int mult_param_value_len(const char *input, size_t avail)
{
  int len;

  for (len = 0; 
       len < (avail - 1) && input[len] != '\r' && input[len+1] != '\n';
       len++);
  if (len == avail - 1)
    return -1;
  else
    return len;
}

static int mult_skip_double_eol(const char **input, size_t *input_len)
{
  int skipped = 0;

  while (!skipped && *input_len >= 4) {
    for ( ; (*input_len) > 0 && **input != '\r'; (*input_len)--, (*input)++);
    if (*input_len >= 4) {
      if ((*input)[1] == '\n' && (*input)[2] == '\r' && (*input)[3] == '\n')
	skipped = 1;
      *input_len -= 4;
      *input += 4;
    }
  }

  return skipped;
}

static void cgi_write_header()
{
  fprintf(stdout, "<?xml version=\"1.0\"");
  if (document->encoding[0]) 
    fprintf(stdout," encoding=\"%s\"", document->encoding);
  fprintf(stdout,"?>\n\n");

  fprintf(stdout,
"<!DOCTYPE html\n\
   PUBLIC \"-//W3C//DTD XHTML 1.0 Strict//EN\"\n\
   \"http://www.w3.org/TR/xhtml1/DTD/xhtml1-strict.dtd\">\n\n");

  fprintf(stdout,
"<html xmlns=\"http://www.w3.org/1999/xhtml\">\n\
  <head>\n\
    <title>html2xhtml - page translated</title>\n\
    <link type=\"text/css\" href=\"/jaf/xhtmlpedia/xhtmlpedia.css\" \
rel=\"stylesheet\"/>\n\
  </head>\n");
  
  fprintf(stdout,
" <body>\n\
    <table class=\"navigation\">\n\
      <tr>\n\
        <td class=\"nav-left\"><a href=\"/jaf/html2xhtml/\">back to main page\
</a></td>\n\
        <td class=\"nav-center\"><a href=\"/jaf/xhtmlpedia/index.html\">\
go to the xhtmlpedia</a></td>\n\
        <td class=\"nav-right\"><a href=\"/jaf/html2xhtml/download.html\">\
download html2xhtml</a></td>\n\
      </tr>\n\
    </table>");

  fprintf(stdout,
"    <div class=\"title\">\n\
      <h1>html2xhtml</h1>\n\
      <p>The document has been converted</p>\n\
    </div>\n");

  fprintf(stdout,
"    <p>The input document has been succesfully converted. If you want\n\
      to save it in a file, copy and paste it in a text editor.\n\
      You can also <a href=\"/jaf/html2xhtml/download.html\">download\n\
      html2xhtml</a> and run it in your computer.</p>\n\
    <pre class=\"document\" xml:space=\"preserve\">\n");
}

static void cgi_write_footer()
{
  fprintf(stdout,
"</pre>\n\
    <p class=\"boxed\">\n\
    <img src=\"/jaf/html2xhtml/h2x.png\" alt=\"html2xhtml logo\" />\n\
      <i>html2xhtml %s</i>, copyright 2001-2008 <a href=\
\"http://www.it.uc3m.es/jaf/index.html\">Jesús Arias Fisteus</a>; \
2001 Rebeca Díaz Redondo, Ana Fernández Vilas\n\
    </p>\n", VERSION);

#ifdef CGI_DEBUG
  cgi_debug_write_state();
#endif

  fprintf(stdout, "</body>\n</html>\n");
}

#ifdef CGI_DEBUG
static void cgi_debug_write_input()
{
  int i;
  int c;

  fprintf(stdout,"Content-Type:%s\n\n","text/plain");

  while (1){
    c=fgetc(stdin);
    if (c==EOF) break;
    fputc(c,stdout);
  }
}

static void cgi_debug_write_state()
{
  fprintf(stdout,"<hr/><p>Internal state:</p>");
  fprintf(stdout,"<ul>");
  fprintf(stdout,"<li>CGI status: %d</li>", cgi_status);
  fprintf(stdout,"<li>HTML output: %d</li>", param_cgi_html_output);
  fprintf(stdout,"</ul>");
}
#endif

#endif