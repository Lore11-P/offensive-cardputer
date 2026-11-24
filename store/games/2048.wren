// name: 2048
// desc: Unisci le tessere e arriva a 2048. Frecce per muovere.
// version: 1.0
// 2048 per ORBIT OS - 240x135, ENTER per ripartire.
var bg = Game.rgb(8, 10, 18)
var panel = Game.rgb(18, 22, 34)
var emptyC = Game.rgb(26, 32, 46)
var fg = Game.rgb(225, 238, 255)
var acc = Game.rgb(90, 220, 255)
var hot = Game.rgb(255, 190, 60)
var dim = Game.rgb(110, 125, 150)

var top = 15
var bx = 37
var by = 22
var cw = 40
var ch = 25
var stepX = 42
var stepY = 27

var state = 0
var grid = [[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]
var score = 0
var locked = false
var moved = false

var tileColors = {
  2: Game.rgb(40, 48, 68),
  4: Game.rgb(54, 66, 94),
  8: Game.rgb(242, 164, 72),
  16: Game.rgb(240, 132, 64),
  32: Game.rgb(234, 100, 70),
  64: Game.rgb(226, 70, 74),
  128: Game.rgb(240, 204, 84),
  256: Game.rgb(236, 184, 62),
  512: Game.rgb(214, 226, 92),
  1024: Game.rgb(116, 224, 236),
  2048: Game.rgb(92, 214, 255)
}

var tileColor = Fn.new { |v|
  var c = tileColors[v]
  if (c == null) {
    c = Game.rgb(255, 96, 128)
  }
  return c
}

var tileText = Fn.new { |v|
  if (v <= 4) {
    return fg
  }
  return bg
}

var spawn = Fn.new {
  var free = []
  for (r in 0...4) {
    for (c in 0...4) {
      if (grid[r][c] == 0) { free.add([r, c]) }
    }
  }
  if (free.count == 0) {
    return
  }
  var spot = free[Game.random(free.count)]
  var v = 2
  if (Game.random(10) == 0) { v = 4 }
  grid[spot[0]][spot[1]] = v
}

var newGame = Fn.new {
  grid = [[0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0], [0, 0, 0, 0]]
  score = 0
  spawn.call()
  spawn.call()
  locked = true
  state = 1
}

var moveLine = Fn.new { |cells|
  var vals = []
  for (cell in cells) {
    var v = grid[cell[0]][cell[1]]
    if (v != 0) { vals.add(v) }
  }
  var out = []
  var i = 0
  while (i < vals.count) {
    if (i + 1 < vals.count && vals[i] == vals[i + 1]) {
      var merged = vals[i] * 2
      out.add(merged)
      score = score + merged
      i = i + 2
    } else {
      out.add(vals[i])
      i = i + 1
    }
  }
  while (out.count < 4) { out.add(0) }
  for (j in 0...4) {
    var r = cells[j][0]
    var c = cells[j][1]
    if (grid[r][c] != out[j]) {
      grid[r][c] = out[j]
      moved = true
    }
  }
}

var canPlay = Fn.new {
  var ok = false
  for (r in 0...4) {
    for (c in 0...4) {
      if (grid[r][c] == 0) { ok = true }
    }
  }
  if (!ok) {
    for (r in 0...4) {
      for (c in 0...3) {
        if (grid[r][c] == grid[r][c + 1]) { ok = true }
      }
    }
    for (r in 0...3) {
      for (c in 0...4) {
        if (grid[r][c] == grid[r + 1][c]) { ok = true }
      }
    }
  }
  return ok
}

var slide = Fn.new { |dir|
  moved = false
  if (dir == 0) {
    for (r in 0...4) { moveLine.call([[r, 0], [r, 1], [r, 2], [r, 3]]) }
  } else if (dir == 1) {
    for (r in 0...4) { moveLine.call([[r, 3], [r, 2], [r, 1], [r, 0]]) }
  } else if (dir == 2) {
    for (c in 0...4) { moveLine.call([[0, c], [1, c], [2, c], [3, c]]) }
  } else {
    for (c in 0...4) { moveLine.call([[3, c], [2, c], [1, c], [0, c]]) }
  }
  if (moved) {
    spawn.call()
    Game.beep(520, 12)
    if (!canPlay.call()) {
      state = 2
      Game.beep(140, 260)
    }
  }
}

var tryMove = Fn.new { |dir|
  if (locked) {
    return
  }
  slide.call(dir)
  locked = true
}

var drawHeader = Fn.new {
  Game.rect(0, 0, Game.width, top, panel)
  Game.text(6, 4, "2048", acc, 2)
  var hud = "PUNTI %(score)"
  Game.text(Game.width - 6 - (hud.count * 6 - 1), 4, hud, hot, 0)
}

var drawTitle = Fn.new {
  Game.rect(0, 0, Game.width, Game.height, bg)
  var title = "2048"
  Game.text((Game.width - (title.count * 12 - 2)) / 2, 18, title, acc, 4)
  Game.text(27, 40, "Unisci i numeri, arriva a 2048.", fg, 0)
  var demo = [2, 8, 32, 128]
  for (i in 0...4) {
    var x = 43 + i * 40
    Game.rect(x, 58, 34, 22, tileColor.call(demo[i]))
    var ds = "%(demo[i])"
    Game.text(x + (34 - (ds.count * 7 - 2)) / 2, 64, ds, tileText.call(demo[i]), 2)
  }
  Game.text(82, 94, "FRECCE: muovi", hot, 0)
  Game.text(45, 112, "ENTER: gioca   ESC: esci", dim, 0)
}

var drawBoard = Fn.new {
  Game.rect(0, 0, Game.width, Game.height, bg)
  Game.rect(33, 18, 174, 114, panel)
  for (r in 0...4) {
    for (c in 0...4) {
      var x = bx + c * stepX
      var y = by + r * stepY
      var v = grid[r][c]
      if (v == 0) {
        Game.rect(x, y, cw, ch, emptyC)
      } else {
        Game.rect(x, y, cw, ch, tileColor.call(v))
        var s = "%(v)"
        Game.text(x + (cw - (s.count * 7 - 2)) / 2, y + 8, s, tileText.call(v), 2)
      }
    }
  }
  drawHeader.call()
  if (state == 2) {
    Game.rect(40, 42, 160, 54, panel)
    Game.rect(40, 42, 160, 2, hot)
    var title = "GAME OVER"
    Game.text((Game.width - (title.count * 7 - 2)) / 2, 48, title, hot, 2)
    var s = "PUNTI %(score)"
    Game.text((Game.width - (s.count * 6 - 1)) / 2, 64, s, fg, 0)
    Game.text(45, 80, "ENTER riprova - ESC esci", dim, 0)
  }
}

newGame.call()
state = 0

while (true) {
  Game.frame()
  if (state == 0) {
    drawTitle.call()
    if (Game.select) {
      newGame.call()
    }
  } else {
    if (state == 1) {
      if (Game.left) {
        tryMove.call(0)
      } else if (Game.right) {
        tryMove.call(1)
      } else if (Game.up) {
        tryMove.call(2)
      } else if (Game.down) {
        tryMove.call(3)
      } else {
        locked = false
      }
    } else if (Game.select) {
      newGame.call()
    }
    drawBoard.call()
  }
  Game.present()
}
