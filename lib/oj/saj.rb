module Oj
  # A SAX style parse handler for JSON hence the acronym SAJ for Simple API for
  # JSON. The Oj::Saj handler class should be subclassed and then used with the
  # Oj.saj_parse() method. The Saj methods will then be called as the file is
  # parsed.
  #
  # @example
  # 
  #  require 'oj'
  #
  #  class MySaj < ::Oj::Saj2
  #    def initialize()
  #      @hash_cnt = 0
  #    end
  #
  #    def start_hash()
  #      @hash_cnt += 1
  #    end
  #  end
  #
  #  cnt = MySaj.new()
  #  File.open('any.json', 'r') do |f|
  #    Oj.saj_parse(cnt, f)
  #  end
  #
  # To make the desired methods active while parsing the desired method should
  # be made public in the subclasses. If the methods remain private they will
  # not be called during parsing.
  #
  #    def hash_start(); end
  #    def hash_end(); end
  #    def array_start(); end
  #    def array_end(); end
  #    def add_value(value); end
  #    def error(message, line, column); end
  #
  class Saj
    # Create a new instance of the Saj handler class.
    def initialize()
    end

    # To make the desired methods active while parsing the desired method should
    # be made public in the subclasses. If the methods remain private they will
    # not be called during parsing.
    private

    def hash_start()
    end

    def hash_end()
    end

    def array_start()
    end

    def array_end()
    end

    def add_value(value)
    end
    
    def error(message, line, column)
    end
    
  end # Saj
end # Oj
