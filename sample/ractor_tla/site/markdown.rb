# frozen_string_literal: true

require "cgi"

# Renders the subset of Markdown that the sample READMEs use: ATX headings,
# paragraphs, pipe tables, fenced code, bullet lists, inline code, bold,
# emphasis, and links.
module RactorSite
  module Markdown
    module_function

    # Renders the section under the heading named title, with that heading
    # at HTML level `level` and deeper headings shifted by the same amount.
    def section(text, title, level:)
      lines = text.lines(chomp: true)
      start = lines.index { |l| l.match?(/\A#+ #{Regexp.escape(title)}\z/) } or
        raise RactorTLA::Failure, "README has no heading #{title.inspect}"
      depth = lines[start][/\A#+/].size
      stop = ((start + 1)...lines.size).find { |i| (m = lines[i][/\A#+(?= )/]) && m.size <= depth && !in_fence?(lines, i) } || lines.size
      render(lines[start...stop], level - depth)
    end

    def in_fence?(lines, index)
      lines[0...index].count { |l| l.start_with?("```") }.odd?
    end

    def render(lines, shift)
      out = []
      i = 0
      while i < lines.size
        line = lines[i]
        if line.start_with?("```")
          body = []
          i += 1
          while i < lines.size && !lines[i].start_with?("```")
            body << lines[i]
            i += 1
          end
          i += 1
          out << "<pre>#{CGI.escapeHTML(body.join("\n"))}</pre>"
        elsif (m = line.match(/\A(#+) (.*)\z/))
          level = (m[1].size + shift).clamp(1, 6)
          id = m[2].gsub(/`/, "").gsub(/[^A-Za-z0-9]+/, "-").downcase.gsub(/\A-|-\z/, "")
          out << "<h#{level} id=\"#{id}\">#{inline(m[2])}</h#{level}>"
          i += 1
        elsif line.start_with?("|")
          rows = []
          while i < lines.size && lines[i].start_with?("|")
            rows << lines[i]
            i += 1
          end
          out << table(rows)
        elsif line.match?(/\A[-*] /)
          items = []
          while i < lines.size && (lines[i].match?(/\A[-*] /) || (lines[i].start_with?("  ") && items.any?))
            if lines[i].match?(/\A[-*] /)
              items << lines[i].sub(/\A[-*] /, "")
            else
              items[-1] = "#{items[-1]} #{lines[i].strip}"
            end
            i += 1
          end
          out << "<ul>#{items.map { |t| "<li>#{inline(t)}</li>" }.join}</ul>"
        elsif line.strip.empty?
          i += 1
        else
          body = []
          while i < lines.size && !lines[i].strip.empty? && !lines[i].match?(/\A(#|\||```|[-*] )/)
            body << lines[i].strip
            i += 1
          end
          out << "<p>#{inline(body.join(" "))}</p>"
        end
      end
      out.join("\n")
    end

    def cells(row)
      row.strip.delete_prefix("|").delete_suffix("|").split(/(?<!\\)\|/).map(&:strip)
    end

    def table(rows)
      head = cells(rows.first)
      body = rows.drop(2).map { |r| "<tr>#{cells(r).map { |c| "<td>#{inline(c)}</td>" }.join}</tr>" }
      "<table><thead><tr>#{head.map { |c| "<th>#{inline(c)}</th>" }.join}</tr></thead><tbody>#{body.join}</tbody></table>"
    end

    def inline(text)
      parts = text.split(/(`[^`]+`)/)
      parts.map do |part|
        if part.start_with?("`") && part.end_with?("`") && part.size > 1
          "<code>#{CGI.escapeHTML(part[1...-1])}</code>"
        else
          CGI.escapeHTML(part)
             .gsub(/\[([^\]]+)\]\(([^)]+)\)/, '<a href="\2">\1</a>')
             .gsub(/&lt;(https?:[^&]+)&gt;/, '<a href="\1">\1</a>')
             .gsub(/\*\*([^*]+)\*\*/, '<strong>\1</strong>')
             .gsub(/(?<![\w*])\*([^*\s][^*]*)\*(?![\w*])/, '<em>\1</em>')
        end
      end.join
    end
  end
end
