require 'bundler/setup'
require 'jekyll'
require 'uri'
require 'json'
require 'fileutils'
require 'nokogiri'

module DocumentationSite
  ROOT = File.expand_path('..', __dir__)

  def self.location(value = ENV.fetch('DOCS_SITE_URL', ''))
    return { 'url' => '', 'baseurl' => '' } if value.empty?
    uri = URI.parse(value)
    unless uri.is_a?(URI::HTTPS) && uri.host && !uri.userinfo && !uri.query && !uri.fragment && !uri.path.match?(%r{//|%2f|%5c}i)
      raise 'DOCS_SITE_URL must be an HTTPS URL without credentials, query, fragment or ambiguous path'
    end
    base = uri.path.sub(%r{/+\z}, '')
    { 'url' => "https://#{uri.host}#{uri.port == 443 ? '' : ':' + uri.port.to_s}", 'baseurl' => base }
  end

  def self.configuration(extra = {})
    Jekyll.configuration({ 'config' => File.join(ROOT, '_config.yml'), 'source' => File.join(ROOT, 'source'),
                           'destination' => File.join(ROOT, '_site'), 'plugins_dir' => [File.join(ROOT, '_plugins')] }.merge(location).merge(extra))
  end

  def self.verify(destination = File.join(ROOT, '_site'))
    manifest = JSON.parse(File.read(File.join(destination, 'build-manifest.json')))
    base = manifest.fetch('baseurl')
    html = Dir.glob(File.join(destination, '**', '*.html')).to_h do |file|
      [file.delete_prefix(destination).tr('\\', '/'), Nokogiri::HTML(File.read(file, encoding: 'UTF-8'))]
    end
    errors = []
    manifest['articles'].each { |article| errors << "Missing article: #{article['url']}" unless html.key?(article['url'] + 'index.html') }
    links = 0
    html.each do |path, document|
      ids = document.css('[id]').map { |element| element['id'] }
      errors << "Duplicate IDs in #{path}" unless ids.uniq.length == ids.length
      document.css('a[href], img[src], script[src], link[href]').each do |element|
        target = element['href'] || element['src']
        next if target.nil? || target.empty? || target.match?(%r{\A(?:[a-z][a-z0-9+.-]*:|//)}i)
        links += 1
        name, fragment = target.split('#', 2)
        name = name.split('?', 2).first.to_s
        resolved = if name.empty?
                     base + path
                   elsif name.start_with?('/')
                     URI::DEFAULT_PARSER.unescape(name)
                   else
                     File.expand_path(URI::DEFAULT_PARSER.unescape(name), File.dirname(base + path)).tr('\\', '/')
                   end
        unless base.empty? || resolved.start_with?(base + '/')
          errors << "#{path}: outside site base #{target}"
          next
        end
        relative = resolved.delete_prefix(base)
        relative += 'index.html' if relative.end_with?('/')
        file = File.join(destination, relative)
        unless File.file?(file)
          errors << "#{path}: missing target #{target}"
          next
        end
        if fragment && html[relative]
          id = URI::DEFAULT_PARSER.unescape(fragment)
          errors << "#{path}: missing anchor #{target}" unless html[relative].css('[id]').any? { |node| node['id'] == id }
        end
      end
    end
    records = JSON.parse(File.read(File.join(destination, 'search-index.json')))
    manifest['articles'].each do |article|
      errors << "Missing search article: #{article['url']}" unless records.any? { |r| r['url'].split('#').first == base + article['url'] }
    end
    raise errors.uniq.join("\n") unless errors.empty?
    puts "Verified #{manifest['articles'].length} articles, #{html.length} HTML pages, #{links} links and #{records.length} search sections"
  end

end

if $PROGRAM_NAME == __FILE__
  Dir.chdir(DocumentationSite::ROOT) do
    case ARGV.shift
    when 'build'
      Jekyll::Site.new(DocumentationSite.configuration).process
      DocumentationSite.verify
    when 'verify'
      DocumentationSite.verify
    when 'serve'
      require 'webrick'
      manifest = JSON.parse(File.read('_site/build-manifest.json'))
      base = manifest.fetch('baseurl')
      server = WEBrick::HTTPServer.new(Port: 4030, BindAddress: '127.0.0.1', AccessLog: [], Logger: WEBrick::Log.new($stderr, WEBrick::Log::WARN))
      server.mount(base + '/', WEBrick::HTTPServlet::FileHandler, File.expand_path('_site'), FancyIndexing: false)
      trap('INT') { server.shutdown }
      trap('TERM') { server.shutdown }
      puts "Preview: http://127.0.0.1:4030#{base}/"
      $stdout.flush
      server.start
    else
      abort 'Usage: bundle exec ruby scripts/site.rb build|verify|serve'
    end
  end
end
