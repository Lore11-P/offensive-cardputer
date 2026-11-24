// name: Snake
// desc: Mangia e cresci senza morderti la coda. Griglia 20x10.
// Snake per ORBIT OS - 240x135, ENTER per giocare/ripartire.
var bg = Game.rgb(8, 10, 18)
var panel = Game.rgb(18, 22, 34)
var grid = Game.rgb(24, 30, 46)
var fg = Game.rgb(225, 238, 255)
var acc = Game.rgb(90, 220, 255)
var bodyC = Game.rgb(110, 240, 160)
var hot = Game.rgb(255, 190, 60)
var dim = Game.rgb(110, 125, 150)
var foodC = Game.rgb(255, 90, 95)

var top = 15
var cell = 12
var cols = 20
var rows = 10

var state = 0
var body = []
var dx = 1
var dy = 0
var foodX = 0
var foodY = 0
var score = 0
var tick = 140
var last = 0
var turned = false

var spawnFood = Fn.new {
  if (body.count >= cols * rows) {
    return
  }
  var placed = false
  while (!placed) {
    foodX = Game.random(cols)
    foodY = Game.random(rows)
    placed = true
    for (i in 0...body.count) {
      if (body[i][0] == foodX && body[i][1] == foodY) {
        placed = false
      }
    }
  }
}

var reset = Fn.new {
  body = []
  body.add([6, 5])
  body.add([5, 5])
  body.add([4, 5])
  dx = 1
  dy = 0
  score = 0
  tick = 140
  turned = false
  spawnFood.call()
  last = Game.millis
}

var drawHeader = Fn.new {
  Game.rect(0, 0, Game.width, top, panel)
  Game.text(6, 3, "SNAKE", acc, 0)
  var hud = "PUNTI %(score)"
  Game.text(Game.width - 6 - (hud.count * 6 - 1), 3, hud, hot, 0)
}

var drawBoard = Fn.new {
  Game.rect(0, top, Game.width, Game.height - top, bg)
  for (c in 0...cols) {
    Game.line(c * cell, top, c * cell, Game.height, grid)
  }
  for (r in 0...rows + 1) {
    Game.line(0, top + r * cell, Game.width, top + r * cell, grid)
  }
  Game.rect(foodX * cell + 2, top + foodY * cell + 2, 8, 8, foodC)
  Game.rect(foodX * cell + 8, top + foodY * cell, 3, 3, hot)
  for (i in 0...body.count) {
    var c = bodyC
    if (i == 0) {
      c = acc
    } else if (i == body.count - 1) {
      c = dim
    }
    Game.rect(body[i][0] * cell + 1, top + body[i][1] * cell + 1, 10, 10, c)
  }
  if (state == 2) {
    Game.rect(44, 48, 152, 46, panel)
    Game.text(89, 53, "GAME OVER", hot, 2)
    var s = "PUNTI %(score)"
    Game.text((Game.width - (s.count * 7 - 2)) / 2, 67, s, fg, 2)
    Game.text(50, 81, "ENTER riparti - ESC esci", dim, 0)
  }
}

var drawTitle = Fn.new {
  Game.rect(0, 0, Game.width, Game.height, bg)
  for (c in 0...cols) {
    Game.line(c * cell, top, c * cell, Game.height, grid)
  }
  Game.text(92, 26, "SNAKE", acc, 4)
  Game.text(38, 56, "Mangia, cresci, non morderti.", fg, 0)
  Game.rect(90, 72, 24, 10, bodyC)
  Game.rect(114, 72, 10, 10, acc)
  Game.rect(150, 72, 8, 8, foodC)
  Game.text(82, 94, "FRECCE: muovi", hot, 0)
  Game.text(48, 108, "ENTER: gioca   ESC: esci", dim, 0)
}

reset.call()
state = 0

while (true) {
  Game.frame()
  if (state == 0) {
    drawTitle.call()
    if (Game.select) {
      reset.call()
      state = 1
    }
  } else {
    if (state == 1) {
      if (Game.left && dx == 0 && !turned) {
        dx = -1
        dy = 0
        turned = true
      }
      if (Game.right && dx == 0 && !turned) {
        dx = 1
        dy = 0
        turned = true
      }
      if (Game.up && dy == 0 && !turned) {
        dx = 0
        dy = -1
        turned = true
      }
      if (Game.down && dy == 0 && !turned) {
        dx = 0
        dy = 1
        turned = true
      }
      if (Game.millis - last >= tick) {
        last = Game.millis
        turned = false
        var hx = body[0][0] + dx
        var hy = body[0][1] + dy
        if (hx < 0 || hx >= cols || hy < 0 || hy >= rows) {
          state = 2
          Game.beep(120, 250)
        } else {
          var ate = hx == foodX && hy == foodY
          var hit = false
          // Se non mangia, la coda si sposta: l'ultima cella e' libera.
          var limit = ate ? body.count : body.count - 1
          for (i in 0...limit) {
            if (body[i][0] == hx && body[i][1] == hy) {
              hit = true
            }
          }
          if (hit) {
            state = 2
            Game.beep(120, 250)
          } else {
            body.insert(0, [hx, hy])
            if (ate) {
              score = score + 1
              if (tick > 70) {
                tick = tick - 4
              }
              spawnFood.call()
              Game.beep(880, 20)
            } else {
              body.removeAt(body.count - 1)
            }
          }
        }
      }
    } else if (Game.select) {
      reset.call()
      state = 1
    }
    drawHeader.call()
    drawBoard.call()
  }
  Game.present()
}
