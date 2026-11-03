require 'json'
require 'pathname'
require 'cgi'
require 'uri'
require 'nokogiri'

module PlatformDocs
  def self.slug(text)
    value = text.strip.downcase.gsub(/[^\p{L}\p{N}\p{M}_\-\s]/, '').gsub(/\s/, '-')
    value.empty? ? 'section' : value
  end

  def self.url_for(id, section)
    parts = id.sub(/\.md\z/, '').split('/')
    parts.pop if parts.last == 'README'
    parts.shift if parts.first == section || %w[tutorials device].include?(section)
    "/#{section}/#{parts.join('/')}/"
  end

  def self.page(site, url, layout, data = {}, content = '')
    page = Jekyll::PageWithoutAFile.new(site, site.source, url.sub(%r{^/}, ''), 'index.md')
    page.content = content
    page.data.merge!(data.merge('layout' => layout, 'permalink' => url))
    site.pages << page
    page
  end

  class Catalog < Jekyll::Generator
    priority :highest

    def generate(site)
      root = File.expand_path('..', site.source)
      platform = File.expand_path('..', root)
      groups = site.data.fetch('catalog').fetch('groups')
      sections = site.data.fetch('catalog').fetch('sections')
      sources = site.data.fetch('components')
      site.config['platform_root'] = platform
      site.config['doc_sources'] = {}
      articles = site.collections.fetch('docs').docs.to_h { |d| [d.relative_path.sub(%r{^_docs/}, ''), d] }
      expected = []

      groups.each do |group|
        group['url'] = "/#{group['section']}/#{group['id']}/"
        group['pages'] = group.fetch('documents').map do |id, title|
          raise "Duplicate article #{id}" if expected.include?(id)
          expected << id
          component = sources.find { |item| item['page'] == id }
          if component
            original = File.expand_path(component.fetch('source'), platform)
            raise 'Component source must stay inside the repository' unless original.start_with?(platform + File::SEPARATOR)
            text = File.read(original, encoding: 'UTF-8').sub(/\A---\r?\n.*?\r?\n---\r?\n/m, '')
            article = PlatformDocs.page(site, PlatformDocs.url_for(id, group['section']), 'document', {}, text)
            asset_dir = File.join(File.dirname(original), 'assets')
            Dir.glob(File.join(asset_dir, '**', '*')).select { |f| File.file?(f) }.each do |file|
              relative = Pathname.new(file).relative_path_from(Pathname.new(asset_dir)).to_s
              directory = File.join('components', File.basename(File.dirname(id)), 'assets', File.dirname(relative))
              asset = Jekyll::StaticFile.new(site, asset_dir, File.dirname(relative), File.basename(file))
              target = '/' + File.join(directory, File.basename(file)).tr('\\', '/')
              asset.define_singleton_method(:destination) { |dest| File.join(dest, target) }
              site.static_files << asset
              site.config['doc_sources'][File.expand_path(file)] = target
            end
          else
            article = articles.fetch(id) { raise "Missing article #{id}" }
            original = File.expand_path(article.path)
          end
          url = PlatformDocs.url_for(id, group['section'])
          source_path = Pathname.new(original).relative_path_from(Pathname.new(platform)).to_s.tr('\\', '/')
          article.data.merge!('title' => title, 'doc_id' => id, 'section' => group['section'],
                             'group' => group['id'], 'group_title' => group['title'], 'permalink' => url,
                             'source_file' => original, 'source_path' => source_path, 'render_with_liquid' => false, 'toc' => [])
          site.config['doc_sources'][original] = url
          { 'title' => title, 'url' => url, 'id' => id }
        end
        group['pages'].each_with_index do |entry, index|
          article = (site.collections['docs'].docs + site.pages).find { |p| p.data['doc_id'] == entry['id'] }
          article.data['doc_previous'] = group['pages'][index - 1] if index.positive?
          article.data['doc_next'] = group['pages'][index + 1]
        end
        if sections.find { |section| section['id'] == group['section'] }.fetch('groups').length > 1
          PlatformDocs.page(site, group['url'], 'section', 'title' => group['title'], 'section' => group['section'], 'group' => group['id'], 'groups' => [group])
        end
      end
      unlisted = articles.keys - expected
      raise "Unlisted articles: #{unlisted.join(', ')}" unless unlisted.empty?

      sections.each do |section|
        section['url'] = "/#{section['id']}/"
        matching = groups.select { |g| section['groups'].include?(g['id']) }
        PlatformDocs.page(site, section['url'], section['id'] == 'examples' ? 'examples' : 'section',
                          'title' => section['title'], 'section' => section['id'], 'groups' => matching)
      end
      site.data['sections'] = sections
      site.data['groups'] = groups
      site.data['article_count'] = expected.length
      site.data['document_links'] = groups.flat_map { |g| g['pages'] }.to_h { |p| [p['id'], p['url']] }
      site.data['section_links'] = sections.to_h { |s| [s['id'], s['url']] }
      site.config['doc_sources'][File.join(site.source, 'index.html')] = '/'
      site.static_files.each do |file|
        site.config['doc_sources'][File.expand_path(file.path)] ||= file.relative_path
      end
      outputs = site.collections['docs'].docs + site.pages + site.static_files
      collisions = outputs.group_by { |item| item.destination(site.dest) }.select { |_path, items| items.length > 1 }
      raise "Conflicting output paths: #{collisions.keys.join(', ')}" unless collisions.empty?
    end
  end

  def self.resolve_link(item, href)
    return href if href.empty? || href.match?(%r{\A(?:[a-z][a-z0-9+.-]*:|//|#)}i)
    pathname, suffix = href.split(/(?=[?#])/, 2)
    site = item.site
    if pathname.start_with?('/')
      return site.baseurl + href
    end
    original = File.expand_path(URI::DEFAULT_PARSER.unescape(pathname), File.dirname(item.data.fetch('source_file')))
    resolved = site.config['doc_sources'][original]
    return site.baseurl + resolved + suffix.to_s if resolved
    platform = site.config.fetch('platform_root')
    if original.start_with?(platform + File::SEPARATOR) && File.exist?(original)
      path = Pathname.new(original).relative_path_from(Pathname.new(platform)).to_s.tr('\\', '/')
      kind = File.directory?(original) ? 'tree' : 'blob'
      return "#{site.config['repository']}/#{kind}/#{site.config['repository_branch']}/#{path}#{suffix}"
    end
    raise "#{item.data['doc_id']}: missing link #{href}"
  end

  def self.enrich(item)
    return unless item.data['doc_id']
    fragment = Nokogiri::HTML::DocumentFragment.parse(item.content)
    ids = Hash.new(0)
    toc = []
    first_title = fragment.at_css('h1')
    fragment.css('h1, h2, h3, h4, h5, h6').each do |heading|
      label = heading.text
      base = slug(label)
      n = ids[base]
      ids[base] += 1
      id = n.zero? ? base : "#{base}-#{n}"
      heading['id'] = id
      if heading == first_title
        heading.name = 'span'
        heading['class'] = 'document-anchor'
        heading.content = ''
        next
      end
      toc << { 'id' => id, 'title' => label, 'level' => heading.name[1].to_i } if %w[h2 h3].include?(heading.name)
      anchor = Nokogiri::XML::Node.new('a', fragment.document)
      anchor['class'] = 'heading-anchor'
      anchor['href'] = "##{id}"
      anchor['aria-label'] = "链接到#{label}"
      anchor.content = '#'
      heading.add_child(anchor)
    end
    fragment.css('a[href], img[src]').each do |element|
      attr = element.name == 'img' ? 'src' : 'href'
      element[attr] = resolve_link(item, element[attr])
    end
    fragment.css('table').each do |table|
      wrapper = Nokogiri::XML::Node.new('div', fragment.document)
      wrapper['class'] = 'table-scroll'
      wrapper['tabindex'] = '0'
      wrapper['role'] = 'region'
      wrapper['aria-label'] = '表格'
      table.replace(wrapper)
      wrapper.add_child(table)
    end
    item.data['toc'].replace(toc)
    item.content = fragment.to_html
  end

  def self.search_records(site)
    (site.collections['docs'].docs + site.pages).filter_map do |item|
      next unless item.data['doc_id']
      fragment = Nokogiri::HTML::DocumentFragment.parse(item.content)
      fragment.css('.heading-anchor').remove
      records = []
      heading = item.data['title']
      url = site.baseurl + item.url
      lines = []
      flush = lambda do
        records << { 'title' => heading, 'page' => item.data['title'], 'group' => item.data['group_title'],
                     'url' => url, 'text' => lines.join(' ').gsub(/\s+/, ' ').strip }
      end
      fragment.children.each do |node|
        if %w[h2 h3].include?(node.name)
          flush.call
          lines = []
          heading = node.text
          url = site.baseurl + item.url + '#' + node['id']
        else
          lines << node.text
        end
      end
      flush.call
      records
    end.flatten
  end
end

Jekyll::Hooks.register [:documents, :pages], :post_convert do |item|
  PlatformDocs.enrich(item)
end

Jekyll::Hooks.register :site, :post_write do |site|
  File.write(File.join(site.dest, 'search-index.json'), JSON.generate(PlatformDocs.search_records(site)))
  manifest = { 'baseurl' => site.baseurl, 'articles' => site.data['groups'].flat_map { |g| g['pages'] },
               'components' => site.data['components'].map { |c| c['source'] } }
  File.write(File.join(site.dest, 'build-manifest.json'), JSON.pretty_generate(manifest))
end
