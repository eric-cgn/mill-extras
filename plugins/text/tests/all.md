Markdown test page
==================

A paragraph with *emphasis*, **strong**, ***both***, _under_ and __double__,
`code spans`, and a [link](http://example.com/ "title") over two lines.
Hard break with two spaces  
and with a backslash\
end. Escapes: \*not emphasis\*, \_, \\, \[x\]. Entities: &copy; &amp; &#65; &#x42; & alone.

Setext two
----------

## ATX heading ##

### Links

Inline <http://example.com/> autolink, <mail@example.com> mail,
reference [full][ref], [collapsed][], [shortcut], and forward [later one][fwd].
An image: ![Mill's windmill](windmill.gif) and a reference image ![alt *text*][img].
Not a link: [nothing here] and a [link with `code` in it](a%20b.html).

[ref]: http://example.com/ref
[collapsed]: <http://example.com/with space> 'title'
[shortcut]: /shortcut
[img]: pics/mill.gif

* * *

- one
- two
  - nested a
  - nested b
    1. deep
    2. deeper
- three

1. first
2. second

7) seven
8) eight

- loose one

- loose two

> A quote with **bold**
continued lazily.
> > nested quote
>
> - list in quote

    indented code
      with <tags> & more

```c
int main(void) { return 0; }   /* <ok> */
```

~~~
tilde fence
~~~

<div align=center>
raw <b>HTML</b> block
</div>

Inline <b>raw</b> HTML and <!-- a comment --> in text.

| Name | Size | Kind |
|:-----|-----:|:----:|
| Mill | 300K | app |
| `a\|b` | 1 | *x* |
| short |

___

[fwd]: http://example.com/forward
